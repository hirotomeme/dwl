#
# ~/.bash_profile
#

[[ -f ~/.bashrc ]] && . ~/.bashrc


 exec dbus-run-session dwl & sleep 1 & pipewire & pipewire-pulse & wireplumber & wait
