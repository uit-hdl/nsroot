# nsroot - Minimalist process isolation tool implemented with Linux namespaces

nsroot is a tool for... It uses XYZ namespaces to acheive ...

## Usage

    Usage: nsroot [OPTION] NEWROOT [COMMAND [ARG]...]
       or: nsroot [OPTION]
    OPTION:
      -v,  --volume          Bind mount a directory into a path under NEWROOT.
                             Syntax: SOURCE:DEST[:OPT] where DEST is relative to
                             NEWROOT. OPT may be 'ro' (read-only), 'rw' (read/write).
                             This option may be specified multiple times.
                             Example values: /home/$USER/private:/mnt
                                             /home/$USER/private:/mnt:ro  # for read-only
      -o,  --old-root=/mnt   Where pivot_root should mount the old root before
                             unmounting it. Path is relative to NEWROOT.
      -r   --read-only       Mount NEWROOT as read-only.
      -k   --keep-old-root   Do not unmount old-root after pivot_root.
      -M   --uid-map         Specify uid-map. See user_namespaces(7) and subuid(5)
                             for details.
      -G   --gid-map         Specify gid-map. See user_namespaces(7) and subgid(5)
                             for details.
      -n   --net             Create a new network namespace.
      -i   --ipc             Create a new IPC namespace.
      -h,  --help

    If no COMMAND is given, run '${SHELL} -i' (default: '/bin/bash -i')

## Use inside an AppImage
AppImage is a technology for ... and can be compared to a `.App` file on a Mac.
The `nsroot`-tool can be used in an AppImage to provide extra isolation via Linux namespaces.

A guide for packaging Firefox with nsroot and AppImage can be found here: **TODO, WARNING, etc.**

Benefits of using nsroot within an AppImage are:
- absolute paths are no longer a problem
- increased security by stricter isolation (especially beneficial if the installed AppImage becomes old/unpatched)
- glibc problem?

## Installation

### Prerequisites

- Linux kernel newer than 3.10 (see *Known issues* about CentOS)

**For CentOS, see *Known issues*.**

### Build and install

    git clone ...
    make && make install

### Testing the installation

Simple test of network and IPC namespaces

    nsroot -ni /                # Use network and IPC namespaces
    ifconfig                    # should return nothing

    curl google.com             # Verify that the network is isolated
    curl: (6) Could not resolve host: google.com

Create a new root file system (using `docker export`):

    # First build a root file system
    docker run --name nsroot-test ubuntu echo
    docker export nsroot-test > nsroot-test.tar
    tar -xf nsroot-test.tar # todo: into `ubuntu` directory

    # Create a directory for the old root filesystem (needed by `pivot_root`)
    mkdir ubuntu/.old_root

Test bind mounting into the new filesystem:

    # Bind mount the `/etc` directory on the host into `/mnt` in the new root
    # with read-only permissions (notice the :ro at the end of the argument to -v)
    nsroot -o /.old_root -v /etc:/mnt:ro ubuntu
    ls /mnt                 # should list the contents of `/etc` on the host
    touch /mnt/test         # should fail
    touch: cannot touch '/mnt/test': Read-only file system

## Known issues
- setuid
- setgid
- bind mounting of /dev, /var and /proc file systems
- mounting `/` with `pivot_root`
- bind mounting a parent directory of `cwd`

### CentOS

Currently, `nsroot` does not work on CentOS.

http://rhelblog.redhat.com/2015/07/07/whats-next-for-containers-user-namespaces/

    sudo grubby --args=”user_namespace.enable=1″ --update-kernel=/boot/vmlinuz-3.10.0-229.4.2.el7.x86_64

Tested and works on Fedora 22

## Roadmap
