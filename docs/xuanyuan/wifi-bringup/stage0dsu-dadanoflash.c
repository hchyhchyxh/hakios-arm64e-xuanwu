/*
 * /sbin/launchd of the small image, when the kernel starts from it (rd=md0 is the small image, no
 * system image in memory): the first program the kernel runs, for a root read from the phone's
 * storage as it is needed.
 *
 * The iOS system volume is the Android DSU image on the phone's storage (tools/dada-dsu.sh), which
 * the loader serves block by block as the partition "dsu" (ufs=2; the kernel's /dev/ufs_dsu,
 * bsd/dev/redwood_ufs.c): only what is read comes off the storage, so the system can be as big as
 * the DSU slot (15 GiB) instead of what fits in the kernel's memory window. This program mounts that
 * volume read only on /flash, makes it the root -- this image goes to /redwood, where the system
 * volume has the directory stage0 mounts it on otherwise (src/stage0.c) -- and runs init from there,
 * as stage0 does: from then on all is as in a boot from the system image in memory.
 */
#include "mini.h"

static int
has_word(const char *hay, const char *needle)
{
	size_t i, j;

	for (i = 0; hay[i] != 0; i++) {
		for (j = 0; needle[j] != 0 && hay[i + j] == needle[j]; j++) {
		}
		if (needle[j] == 0) {
			return 1;
		}
	}
	return 0;
}

int
main(int argc, char **argv, char **envp)
{
	char *init_argv[] = { "/redwood/sbin/init", NULL };
	static const char switch_to[] = "/flash redwood";
	int fd = open("/dev/console", O_RDWR, 0), tries;

	(void)argc, (void)argv;
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	if (fd > 2) {
		close(fd);
	}
	printf("\n=== stage0dsu: the system volume from the phone's storage (/dev/ufs_dsu) ===\n");
	/* (the node is made before the first process runs; a few tries in case it is not there yet) */
	for (tries = 0; tries < 50; tries++) {
		if (mount("apfs", "/flash", MNT_RDONLY, (void *)"/dev/ufs_dsu") == 0) {
			break;
		}
		usleep(100000);
	}
	if (tries == 50) {
		printf("stage0dsu: cannot mount /dev/ufs_dsu on /flash: %s\n", strerror(errno));
	} else if (sysctlbyname("kern.redwood_switch_root", NULL, NULL, switch_to, sizeof(switch_to) - 1) != 0) {
		printf("stage0dsu: switching the root: %s\n", strerror(errno));
	} else {
		chdir("/");
		printf("stage0dsu: the system volume is the root, this image on /redwood\n");
		execve(init_argv[0], init_argv, envp);
		printf("stage0dsu: cannot run %s: %s\n", init_argv[0], strerror(errno));
	}
	{
		/* test payloads without a DSU: keep the machine alive instead of halting */
		char args[1024];
		size_t args_len = sizeof(args) - 1;

		if (sysctlbyname("kern.bootargs", args, &args_len, NULL, 0) == 0) {
			args[args_len] = 0;
			if (has_word(args, "dadanoflash=1")) {
				printf("stage0dsu: dadanoflash=1: no system volume, staying in this process\n");
				for (;;) {
					usleep(1000000);
				}
			}
		}
	}
	reboot(0x08 /* RB_HALT */);
	return 1;
}
