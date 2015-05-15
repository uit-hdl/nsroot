/*
  The general structure and some code is taken from: http://man7.org/linux/man-pages/man7/user_namespaces.7.html
*/

#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <getopt.h>
#include <string.h>
#include <alloca.h>

extern int pivot_root(const char *, const char *);

const char usage_string[] = "\
Usage: %s [OPTION] NEWROOT [COMMAND [ARG]...]\n\
   or: %s [OPTION]\n\
OPTION:\n\
  -v,  --volume          Bind mount a directory into a path under NEWROOT.\n\
                         Syntax: SOURCE:DEST[:OPT] where DEST is relative to\n\
                         NEWROOT. OPT may be 'ro' (read-only), 'rw' (read/write).\n\
                         This option may be specified multiple times.\n\
                         Example values: /home/$USER/private:/mnt\n\
                                         /home/$USER/private:/mnt:ro  # for read-only\n\
  -o,  --old-root=/mnt   Where pivot_root should mount the old root before\n\
                         unmounting it. Path is relative to NEWROOT.\n\
  -h,  --help\n\
\n\
If no COMMAND is given, run '${SHELL} -i' (default: '%s -i')\n\
\n\n\
Examples:\n\
    <todo>\n\
\n\
";

char *default_shell() {
  char *sh = getenv("SHELL");
  if(sh == NULL) {
    sh = "/bin/sh";
  }
  return sh;
}

void print_usage(char *exec_name) {
  printf(usage_string, exec_name, exec_name, default_shell());
}

#define STACK_SIZE (1024*1024)

static char child_stack[STACK_SIZE];

typedef struct mount mount_t;
struct mount {
  mount_t *next;
  char *source;
  char *target;
  char *filesystemtype;
  unsigned long mountflags;
  const void *data;
};

typedef struct args args_t;
struct args {
  int pipe_fd[2];
  char **argv;
  char *new_root;
  char *old_root;
  enum {SR_PIVOT_ROOT, SR_CHROOT} switch_root_method;
  mount_t *user_bind_mounts;
  unsigned int clone_flags;
};

mount_t define_bind_mount(char *source, char *target) {
  mount_t m = {
    .next = NULL,
    .source = source,
    .target = target,
    .filesystemtype = NULL,
    .mountflags = MS_BIND,
    .data = NULL,
  };
  return m;
}

int mount_all(mount_t *mo) {
  for(mount_t *m = mo; m != NULL; m = m->next) {
    int ret = mount(m->source, m->target, m->filesystemtype, m->mountflags, m->data);
    if(ret) return ret;
  }
  return 0;
}

void insert_mount(mount_t **mounts, mount_t *new_mount) {
  new_mount->next = *mounts;
  *mounts = new_mount;
}

void fail(char *what) {
  fprintf(stderr, "Error: %s failed (errno: %d): %s\n", what, errno, strerror(errno));
  exit(1);
}


static int child_fun(void *_arg) {
  args_t *args = (args_t *) _arg;
  close(args->pipe_fd[1]);

  char ch;
  if(read(args->pipe_fd[0], &ch, 1)) {
    fail("reading pipe");
  }

  errno = 0;
  if(mount_all(args->user_bind_mounts)) {
    fail("mount");
  };

  char new_root_abs[PATH_MAX];

  if(realpath(args->new_root, new_root_abs) == NULL) {
    fail("resolving new root directory");
  }

  switch(args->switch_root_method) {
  case SR_CHROOT:
    errno = 0;
    if(chroot(new_root_abs)) {
      fail("chroot");
    }
    if (chdir("/")) {
      fail("chdir(\"/\") after chroot");
    }
    break;
  case SR_PIVOT_ROOT:
    ;
    char old_root_abs[PATH_MAX];
    if(args->old_root[0] != '/') {
      fail("old root should be an absolute path");
    }
    int ret = snprintf(old_root_abs, sizeof(old_root_abs), "%s%s", new_root_abs, args->old_root);
    if(ret < 0 || ret >= sizeof(old_root_abs)) {
      fail("snprintf: concatenate old root and new root paths");
    }
    errno = 0;
    if(mount(new_root_abs, new_root_abs, NULL, MS_BIND, NULL)) {
      fail("mount");
    }
    errno = 0;
    if(pivot_root(new_root_abs, old_root_abs) != 0) {
      fail("pivot_root");
    }
    if (chdir("/")) {
      fail("chdir(\"/\") after pivot_root");
    }
    if(mount("", args->old_root, "dontcare", MS_REC | MS_PRIVATE, "")) {
      fail("create private mount over old root");
    }
    if(umount2(args->old_root, MNT_DETACH)) {
      fail("umount2(old_root)");
    }
    break;
  }

  if(execvp(args->argv[0], args->argv)) {
    fail("execvp");
  }
  fail("this never happens");
  return -1;
}


int run(args_t *args) {

  pipe(args->pipe_fd); // fail

  pid_t child_pid = clone(child_fun, child_stack + STACK_SIZE, args->clone_flags | SIGCHLD, args);

  // todo: set gid/uid maps

  close(args->pipe_fd[1]);
  return waitpid(child_pid, NULL, 0);
}

void argument_error(char *err) {
  printf("chrootns: %s See '--help' for details.\n", err);
  exit(-1);
}

int main(int argc, char *argv[], char *envp[]) {
  int flags = CLONE_NEWUSER | CLONE_NEWNS;

  args_t args = {
    .clone_flags = flags,
    .argv = NULL,
    .new_root = NULL,
    .user_bind_mounts = NULL,
    .switch_root_method = SR_CHROOT,
    .old_root = "/mnt",
  };

  while(1) {
    int option_index = 0;
    static struct option long_options[] = {
      {"help", no_argument, 0, 'h'},
      {"volume", required_argument, 0, 'v'},
      {"old-root", required_argument, 0, 'o'},
    };

    int c = getopt_long(argc, argv, "hv:o:",
                        long_options, &option_index);
    if(c == -1) break;
    switch(c) {
    case 'h':
      print_usage(argc > 0 ? argv[0] : "nsroot");
      exit(1);
      break;
    case 'v':
      ;
      char *msg = "Invalid parameter to -v,--volume.";
      char *src, *dest, *opt;
      char **stringp = &optarg;
      unsigned int flags = 0;
      src = strsep(stringp, ":");
      if(src == NULL) argument_error(msg);
      dest = strsep(stringp, ":");
      if(dest == NULL) argument_error(msg);
      opt = strsep(stringp, ":");
      if(opt != NULL) {
        if(strcmp(opt, "ro") == 0) {
          flags |= MS_RDONLY;
        } else if(strcmp(opt, "rw") == 0) {
          // default - ignore
        } else {
          argument_error(msg);
        }
        if(strsep(stringp, ":") != NULL) argument_error(msg);
      }
      mount_t *m = alloca(sizeof(mount_t));
      *m = define_bind_mount(src, dest);
      m->mountflags |= flags;
      insert_mount(&args.user_bind_mounts, m);
      args.switch_root_method = SR_PIVOT_ROOT;
      //printf("src: %s, dest: %s, opt: %s\n", src, dest, opt);
      break;
    case 'o':
      ;
      args.old_root = optarg;
      if(args.old_root[0] != '/') {
        argument_error("old-root must be an absolute path inside NEWROOT.");
      }
      args.switch_root_method = SR_PIVOT_ROOT;
      break;
    }
  }

  if(optind == argc) {
    args.new_root = ".";
  } else {
    args.new_root = argv[optind];
    optind++;
  }

  if(optind < argc) {
    args.argv = &argv[optind];
  } else {
    char *default_argv[] = {default_shell(), "-i", NULL};
    args.argv = default_argv;
  }

  int ret = run(&args);
  return ret;
}
