#
# Regular cron jobs for the acpi-backlightd package
#
0 4	* * *	root	[ -x /usr/bin/acpi-backlightd_maintenance ] && /usr/bin/acpi-backlightd_maintenance
