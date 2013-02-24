# ashttpd - async sendfile http daemon

Copyright (c) 2010 Gianni Tedesco

## INTRODUCTION

ashttpd was intended as a test rig for kernel asynchronous sendfile support.
It is (yet another) simple http daemon for serving up static content. The main
aims are simplicity and efficiency. It differs from other http daemons in that
the backing store is not the VFS but a proprietary database. The benefits of
this approach are performance - when the cache is warm, an open() syscall has
a noticable performance impact. In the future this will also allow for atomic
replacements of entire webroots.

## BUILDING

This might need a few tweaks to build on a non-linux platform but it should
be doable. All the platform specific stuff has POSIX fallbacks but they've not
been tested.

The main linux specific bits are just performance enhancements: accept4(),
fallocate(), epoll, (no kqueue module for example, but it can fallback to
poll or be written by a suitably motivated individual).

## RUNNING

To build a webroot run mkroot, for example:

 $ ./mkroot /var/www www.webroot

Then the server is run with:
 $ ./httpd www.webroot

The httpd binary takes one (optional) commandline argument which selects the
I/O model to be used:

 - sync - traditional synchronous IO
 - sendfile - recommended
 - dio - O\_DIRECT kernel AIO
 - async - kernel AIO, will only be really async if kernel is patched
 - async-sendfile - kernel AIO sendfile, requires patch to kernel and libaio
 
For example:

 $ ./httpd www.webroot sync

If you like and use this software then press [<img src="http://www.paypalobjects.com/en_US/i/btn/btn_donate_SM.gif">](https://www.paypal.com/cgi-bin/webscr?cmd=_donations&business=gianni%40scaramanga%2eco%2euk&lc=GB&item_name=Gianni%20Tedesco&item_number=scaramanga&currency_code=GBP&bn=PP%2dDonationsBF%3abtn_donateCC_LG%2egif%3aNonHosted) to donate towards its development progress and email me to say what features you would like added.
