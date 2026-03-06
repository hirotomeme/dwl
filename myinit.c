#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

#define MAX_SERVICES 8

typedef struct {
    const char *path;
    char *const argv[6];       // достаточно для наших нужд
    pid_t pid;
    int respawn;               // 1 = перезапускать при смерти
    int once;                  // запускать только один раз
} Service;

Service services[] = {
    { "/usr/lib/udev/udevd", 
      { "udevd", "--daemon", NULL }, 
      0, 0, 1 },

    { "/usr/bin/dbus-daemon", 
      { "dbus-daemon", "--system", "--nofork", NULL }, 
      0, 1, 0 },

    { "/usr/lib/iwd/iwd", 
      { "iwd", NULL }, 
      0, 1, 0 },

    { "/usr/lib/elogind/elogind", 
      { "elogind", "--daemon", NULL }, 
      0, 1, 0 },

    { "/sbin/agetty", 
      { "agetty", "--noclear", "38400", "tty1", "linux", NULL }, 
      0, 1, 0 },

    // можно добавить ещё tty2, tty3 если нужно
    // { "/sbin/agetty", { "agetty", "38400", "tty2", "linux", NULL }, 0, 1, 0 },

    { NULL, {NULL}, 0, 0, 0 }   // конец списка
};

static void sigchld_handler(int sig) {
    // ничего не делаем — просто прерываем pause()
    (void)sig;
}

static void reap_children(void) {
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; services[i].path; i++) {
            if (services[i].pid == pid) {
                if (WIFEXITED(status) || WIFSIGNALED(status)) {
                    if (services[i].respawn && !services[i].once) {
                        fprintf(stderr, "[init] %s died → restarting\n", services[i].argv[0]);
                        services[i].pid = 0;  // будет перезапущен в главном цикле
                    } else {
                        fprintf(stderr, "[init] %s exited (not respawning)\n", services[i].argv[0]);
                        services[i].pid = -1; // пометили как завершённый
                    }
                }
                break;
            }
        }
    }
}

int main(void) {
    // PID 1 почти не должен получать сигналы
    signal(SIGINT,  SIG_IGN);
    signal(SIGHUP,  SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGCHLD, sigchld_handler);

    // Монтируем базовые ФС (если ещё не смонтированы)
    mount("proc",  "/proc", "proc",  0, NULL);
    mount("sysfs", "/sys",  "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mount("tmpfs", "/run",  "tmpfs",  MS_NOSUID|MS_NODEV|MS_NOEXEC, NULL);
    mount("tmpfs", "/tmp",  "tmpfs",  MS_NOSUID|MS_NODEV, NULL);

    // перенаправляем stdout/stderr на консоль
    int cons = open("/dev/console", O_RDWR);
    if (cons >= 0) {
        dup2(cons, 0);
        dup2(cons, 1);
        dup2(cons, 2);
        if (cons > 2) close(cons);
    }

    fprintf(stderr, "[myinit] starting (PID 1)\n");

    // Первый запуск всех сервисов
    for (int i = 0; services[i].path; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            // ребёнок
            umask(0);
            execv(services[i].path, services[i].argv);
            fprintf(stderr, "[init] execv failed: %s → %s\n", services[i].path, strerror(errno));
            _exit(127);
        } else if (pid > 0) {
            services[i].pid = pid;
            fprintf(stderr, "[init] started %s (pid %d)\n", services[i].argv[0], pid);
        } else {
            fprintf(stderr, "[init] fork failed for %s\n", services[i].path);
        }
    }

    // Главный цикл PID 1
    while (1) {
        pause();               // ждём сигнала (в основном SIGCHLD)
        reap_children();

        // Проверяем, нужно ли перезапускать упавшие сервисы
        for (int i = 0; services[i].path; i++) {
            if (services[i].respawn && services[i].pid == 0) {
                pid_t pid = fork();
                if (pid == 0) {
                    umask(0);
                    execv(services[i].path, services[i].argv);
                    _exit(127);
                } else if (pid > 0) {
                    services[i].pid = pid;
                    fprintf(stderr, "[init] restarted %s (pid %d)\n", services[i].argv[0], pid);
                }
            }
        }
    }

    // до сюда обычно не доходим
    sync();
    reboot(RB_POWER_OFF);
    return 0;
}
