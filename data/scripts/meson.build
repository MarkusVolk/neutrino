if boxtype == 'generic'
	install_data('audio-output.lua', install_dir : tuxboxdatadir / 'neutrino/scripts')
endif
scripts = ['backup.sh', 'restore.sh']
# the boxes that write their images with ofgwrite; the others have slot-update or no slots
if boxtype != 'generic'
	scripts += ['backup_flash.sh', 'restore_flash.sh', 'ofgwrite_caller', 'flash_slot.sh']
endif
foreach script : scripts
	configure_file(input : script + '.in', output : script, configuration : dirs_conf, install_dir : bindir, install_mode : 'rwxr-xr-x')
endforeach
install_data('install.sh', install_dir : bindir, install_mode : 'rwxr-xr-x')
