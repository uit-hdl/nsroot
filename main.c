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
#include <stdbool.h>
#include <fcntl.h>

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
  -r   --read-only       Mount NEWROOT as read-only.\n\
  -k   --keep-old-root   Do not unmount old-root after pivot_root.\n\
  -M   --uid-map         Specify uid-map. See user_namespaces(7) and subuid(5)\n\
                         for details.\n\
  -G   --gid-map         Specify gid-map. See user_namespaces(7) and subgid(5)\n\
                         for details.\n\
  -n   --net             Create a new network namespace.\n\
  -i   --ipc             Create a new IPC namespace.\n\
  -h,  --help\n\
\n\
If no COMMAND is given, run '${SHELL} -i' (default: '%s -i')\n\
\n\
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

typedef struct mount mount_args;
struct mount {
  mount_args *next;
  char *source;
  char *target;
  char *filesystemtype;
  unsigned long mountflags;
  const void *data;
};

typedef struct args nsroot_args;
struct args {
  int pipe_fd[2];
  char **argv;
  char *new_root;
  char *old_root;
  char *uid_map;
  char *gid_map;
  enum {SR_PIVOT_ROOT, SR_CHROOT} switch_root_method;
  mount_args *user_bind_mounts;
  unsigned int clone_flags;
  bool read_only_root;
  bool keep_old_root;
};

mount_args define_bind_mount(char *source, char *target) {
  mount_args m = {
    .next = NULL,
    .source = source,
    .target = target,
    .filesystemtype = NULL,
    .mountflags = MS_BIND,
    .data = NULL,
  };
  return m;
}

void fail(char *what) {
  fprintf(stderr, "Error: %s failed (errno: %d): %s\n", what, errno, strerror(errno));
  exit(1);
}

int join_paths(char *buffer, int size, char *a, char *b) {
  // todo: bug when a or b == "/"
  if(b[0] == '/') {
    b++;
  }
  int a_len = strlen(a);
  if(a[a_len-1] == '/') a[a_len-1] = 0x0;
  int ret = snprintf(buffer, size, "%s/%s", a, b);
  if (ret < 0 || ret >= size) {
    return -1;
  } else {
    return 0;
  }
}

int mount_all(mount_args *mo, char *source_prefix, char *target_prefix) {
  int ret;
  for(mount_args *m = mo; m != NULL; m = m->next) {
    char source_path_buf[PATH_MAX];
    char target_path_buf[PATH_MAX];
    char *source_path = mo->source;
    char *target_path = mo->target;
    if(source_prefix != NULL) {
      if(join_paths(source_path_buf, sizeof(source_path_buf), source_prefix, mo->source)) {
        return -1;
      }
      source_path = source_path_buf;
    }
    if(target_prefix != NULL) {
      if(join_paths(target_path_buf, sizeof(target_path_buf), target_prefix, mo->target)) {
        return -1;
      }
      target_path = target_path_buf;
    }
    ret = mount(source_path, target_path, m->filesystemtype, m->mountflags, m->data);
    if(ret) return ret;
    if (m->mountflags & MS_RDONLY) {
      ret = mount("", target_path, NULL, MS_RDONLY | MS_REMOUNT | MS_BIND, NULL);
      if(ret) return ret;
    }
  }
  return 0;
}

void insert_mount(mount_args **mounts, mount_args *new_mount) {
  new_mount->next = *mounts;
  *mounts = new_mount;
}


static int child_fun(void *_arg) {
  nsroot_args *args = (nsroot_args *) _arg;
  close(args->pipe_fd[1]);

  char ch;
  if(read(args->pipe_fd[0], &ch, 1)) {
    fail("reading pipe");
  }

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
    if(args->read_only_root) {
      errno = 0;
      if(mount("", new_root_abs, NULL, MS_BIND | MS_RDONLY | MS_REMOUNT, NULL)) {
        fail("remount NEWROOT readonly");
      }
    }
    errno = 0;
    if(pivot_root(new_root_abs, old_root_abs) != 0) {
      fail("pivot_root");
    }
    errno = 0;
    if (chdir("/")) {
      fail("chdir(\"/\") after pivot_root");
    }
    errno = 0;
    if(mount_all(args->user_bind_mounts, args->old_root, NULL)) {
      fail("bind mount user volumes");
    };
    if(!args->keep_old_root) {
      errno = 0;
      if(mount("", args->old_root, "dontcare", MS_REC | MS_PRIVATE, "")) {
        fail("create private mount over old root");
      }
      errno = 0;
      if(umount2(args->old_root, MNT_DETACH)) {
        fail("umount2(old_root)");
      }
    }
    break;
  }

  if(execvp(args->argv[0], args->argv)) {
    fail("execvp");
  }
  fail("this never happens");
  return -1;
}

int write_file(char *path, char *contents) {
  int f = open(path, O_WRONLY | O_SYNC);
  if(f == -1) return -1;
  int ret = dprintf(f, "%s", contents);
  if(ret < 0 || ret == EOF) return -1;
  return close(f);
}

void replace(char *str, char old, char new) {
  for(int i = 0; str[i] != 0; i++) {
    if(str[i] == old) {
      str[i] = new;
    }
  }
}

int run(nsroot_args *args) {
  int ret;
  char *error_msg;

  if(pipe(args->pipe_fd)) fail("creating pipe");

  pid_t child_pid = clone(child_fun, child_stack + STACK_SIZE, args->clone_flags | SIGCHLD, args);

  if(child_pid == -1) {
    fail("clone");
  }

  char path[PATH_MAX];

  if(args->uid_map != NULL) {
    ret = snprintf(path, sizeof(path), "/proc/%d/uid_map", child_pid);
    if(ret < 0 || ret > sizeof(path)) {
      error_msg = "snprintf"; goto fail;
    }
    if (write_file(path, args->uid_map)) {
      error_msg = "writing uid_map"; goto fail;
    }
  }

  if(args->gid_map != NULL) {
    snprintf(path, sizeof(path), "/proc/%d/gid_map", child_pid);
    if(ret < 0 || ret > sizeof(path)) {
      error_msg = "snprintf"; goto fail;
    }
    if(write_file(path, args->gid_map)) {
      error_msg = "writing gid_map"; goto fail;
    }
  }

  close(args->pipe_fd[1]);
  return waitpid(child_pid, NULL, 0);

fail:
  kill(child_pid, SIGKILL);
  close(args->pipe_fd[1]);
  waitpid(child_pid, NULL, 0);
  fail(error_msg);
  return -1; // never happens
}

void argument_error(char *err) {
  printf("chrootns: %s See '--help' for details.\n", err);
  exit(-1);
}

int main(int argc, char *argv[], char *envp[]) {
  int flags = CLONE_NEWUSER | CLONE_NEWNS;

  nsroot_args args = {
    .clone_flags = flags,
    .argv = NULL,
    .new_root = NULL,
    .uid_map = NULL,
    .gid_map = NULL,
    .user_bind_mounts = NULL,
    .switch_root_method = SR_CHROOT,
    .old_root = "/mnt",
    .keep_old_root = false,
  };

  while(1) {
    int option_index = 0;
    static struct option long_options[] = {
      {"help", no_argument, 0, 'h'},
      {"volume", required_argument, 0, 'v'},
      {"old-root", required_argument, 0, 'o'},
      {"read-only", no_argument, 0, 'r'},
      {"keep-old-root", no_argument, 0, 'k'},
      {"uid-map", required_argument, 0, 'M'},
      {"gid-map", required_argument, 0, 'G'},
      {"net", no_argument, 0, 'n'},
      {"ipc", no_argument, 0, 'i'}
    };

    int c = getopt_long(argc, argv, "hv:o:rkM:G:ni",
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
      mount_args *m = alloca(sizeof(mount_args));
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
    case 'r':
      args.read_only_root = true;
      args.switch_root_method = SR_PIVOT_ROOT;
      break;
    case 'k':
      args.keep_old_root = true;
      args.switch_root_method = SR_PIVOT_ROOT;
      break;
    case 'M':
      replace(optarg, ',', '\n');
      args.uid_map = optarg;
      break;
    case 'G':
      replace(optarg, ',', '\n');
      args.gid_map = optarg;
      break;
    case 'n':
      args.clone_flags |= CLONE_NEWNET;
      break;
    case 'i':
      args.clone_flags |= CLONE_NEWIPC;
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
