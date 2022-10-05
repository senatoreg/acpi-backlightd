#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <error.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>

#define EVENT_BUFFER_SIZE 128
#define BACKLIGHT_BUFFER_SIZE 16

typedef struct _backlight_info {
    int backlight_fd;
    int acpid_fd;
    int max_backlight;
    int step;
    int idle_factor;
    struct pollfd *pfds;
} backlight_info_t;

int
acpi_open(const char* name) {
    int fd;
    int r;
    struct sockaddr_un addr;

    if (strnlen(name, sizeof(addr.sun_path)) > sizeof(addr.sun_path) - 1) {
        return -1;
    }
    
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
	return fd;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    sprintf(addr.sun_path, "%s", name);
    /* safer: */
    /*strncpy(addr.sun_path, name, sizeof(addr.sun_path) - 1);*/

    r = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (r < 0) {
	close(fd);
	return r;
    }

    return fd;
}

int
setup_acpi(char* acpid_socketfile, backlight_info_t* bl_info) {
    /* open the socket */
    bl_info->acpid_fd = acpi_open(acpid_socketfile);
    if (bl_info->acpid_fd < 0) 
        return EXIT_FAILURE;

    bl_info->pfds->fd = bl_info->acpid_fd;
    bl_info->pfds->events = POLLIN;

    return 0;
}

int
close_acpi(backlight_info_t* bl_info) {
    return close(bl_info->acpid_fd);
}

int
setup_backlight(char* bl_filename, char* max_bl_filename, backlight_info_t* bl_info) {
    int err = 0;
    char buffer[BACKLIGHT_BUFFER_SIZE];
    int max_fd;

    max_fd = open(max_bl_filename, O_RDONLY);
    if (max_fd < 0) {
	err = EXIT_FAILURE;
	goto exit;
    }
    err = pread(max_fd, buffer, BACKLIGHT_BUFFER_SIZE, 0);
    if (err < 0) {
	err = EXIT_FAILURE;
	goto exit;
    }
    bl_info->max_backlight = atoi(buffer);
    err = close(max_fd);
    if (err < 0) {
	err = EXIT_FAILURE;
	goto exit;
    }

    /* open the file */
    bl_info->backlight_fd = open(bl_filename, O_RDWR);
    if (bl_info->backlight_fd < 0) {
	err = EXIT_FAILURE;
    }

 exit:
    return err;
}

int
close_backlight(backlight_info_t* bl_info) {
    return close(bl_info->backlight_fd);
}

int
set_backlight0(double i, backlight_info_t* bl_info) {
    char buffer[BACKLIGHT_BUFFER_SIZE];
    double cur_bl;
    int err = 0;

    if ((err = pread(bl_info->backlight_fd, buffer, BACKLIGHT_BUFFER_SIZE, 0)) < 0)
	goto exit;
    
    cur_bl = atof(buffer);

    cur_bl *= i;
    
    if (cur_bl > bl_info->max_backlight)
	cur_bl = bl_info->max_backlight;
    else if (cur_bl < 0)
	cur_bl = 1;

    {
	sprintf(buffer, "%d\n", (int)cur_bl);
	err = pwrite(bl_info->backlight_fd, &buffer, BACKLIGHT_BUFFER_SIZE, 0);
    }
    
 exit:
    return err;
}

int
set_backlight(int i, backlight_info_t* bl_info) {
    char buffer[BACKLIGHT_BUFFER_SIZE];
    int cur_bl;
    int err = 0;

    if ((err = pread(bl_info->backlight_fd, buffer, BACKLIGHT_BUFFER_SIZE, 0)) < 0)
	goto exit;
    
    cur_bl = atoi(buffer);
    //i = i * (cur_bl * 100 / max_backlight) / 100;

    // cur_bl *= i;
    // cur_bl += 1;

    cur_bl += i;
    // if (cur_bl + i < 0)
    // 	cur_bl = 0;
    // else if (cur_bl > bl_info->max_backlight)
    // 	cur_bl = bl_info->max_backlight;
    
    if (cur_bl > bl_info->max_backlight)
	cur_bl = bl_info->max_backlight;
    else if (cur_bl < 0)
	cur_bl = 0;

    {
	sprintf(buffer, "%d\n", (int)cur_bl);
	err = pwrite(bl_info->backlight_fd, &buffer, BACKLIGHT_BUFFER_SIZE, 0);
    }
    
 exit:
    return err;
}

int
acpi_event_handler(backlight_info_t* bl_info){
    int err = 0;
    char event[EVENT_BUFFER_SIZE];
    int step = bl_info->step;
    struct pollfd* pfds = bl_info->pfds;
    
    while (1) {
        /* read and handle an event */
	err = poll(pfds, 1, -1);

	if (pfds->revents && POLLIN) {
	    err = read(pfds->fd, event, EVENT_BUFFER_SIZE);
	    if (err > 0) { 
		if ((err=strncmp(event,"video/brightnessdown BRTDN",26)) == 0) {
		    if ((err=set_backlight(-step, bl_info)) < 0) break;
		} else if ((err=strncmp(event,"video/brightnessup BRTUP",24)) == 0) {
		    if ((err=set_backlight(step, bl_info)) < 0) break;
		} else if ((err=strncmp(event,"ac_adapter ACPI0003:00 00000080 00000000",40)) == 0) {
		    if ((err=set_backlight0(0.5, bl_info)) < 0) break;
		} else if ((err=strncmp(event,"ac_adapter ACPI0003:00 00000080 00000001",40)) == 0) {
		    if ((err=set_backlight0(2.0, bl_info)) < 0) break;
		}
	    }
	}
    }

    return err;
}

int
main(int argc, char** argv, char** envp) {
    char *acpid_socket = "/var/run/acpid.socket";
    char *bl_filename = "/sys/class/backlight/intel_backlight/brightness";
    char *max_bl_filename = "/sys/class/backlight/intel_backlight/max_brightness";
    int err;
    char event[EVENT_BUFFER_SIZE];
    backlight_info_t* bl_info;

    bl_info = malloc(sizeof(backlight_info_t));
    
    bl_info->pfds = malloc(sizeof(struct pollfd));

    if ( (err=setup_backlight(bl_filename, max_bl_filename, bl_info)) != 0 ) {
        return err;
    }

    bl_info->step = bl_info->max_backlight * 0.01;
        
    if ( (err=setup_acpi(acpid_socket, bl_info)) != 0 )
        return err;

    err = acpi_event_handler(bl_info);

    close_acpi(bl_info);
    close_backlight(bl_info);

    free(bl_info->pfds);
    free(bl_info);

    return err;
}
