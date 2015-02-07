# Svndumpsanitizer

## Home page

This is an abridged version of the documentation available on the [project home page.](http://miria.homelinuxserver.org/svndumpsanitizer) For the full version please go there.

## Background

This is the home page for my tool svndumpsanitizer. It's a small project born from my experiences with the official subversion tool "svndumpfilter". Svndumpfilter unfortunately does not work with every valid repository, and even though I can't vouch for my program either, I have certainly tried to make it that way. If it doesn't work with some valid repository, that is to be considered a bug. I know it can handle all the files I've thrown at it, that svndumpfilter couldn't.

## Misc info

The program has been tested on Linux (i386 and x86_64 architectures) and should work out-of-the-box on any system using the GNU toolchain. It uses only standard libraries and should be easily portable, though. The only thing that might cause some snags is the 64 bit file API. (As of 1.0.2 it contains a modification by $ergi0 that should make it possible to
build under Windows. I haven't tested that myself, though.)

To compile it, just run:

```sh
$ gcc svndumpsanitizer.c -o svndumpsanitizer
```
For complete usage instructions run:
```sh
$ ./svndumpsanitizer --help
```
A typical command would look something like this:
```sh
$ ./svndumpsanitizer --infile huge_mess.dump --outfile repo1.dump --include trunk/repo1 tags/repo1 branches/my_really_important_stuff --drop-empty
```
## What svndumpsanitizer *doesn't* do

To save yourself time and frustration by trying to use this tool for something it was never intended to do, please notice that it does not:

- **Repair broken dump files.** Svndumpsanitizer assumes that it's reading valid data. If you try to give it a broken dump file, the principle of "Garbage in, garbage out" applies.
- **Work with partial dumps.** There is a good technical reason for this. The short version is that svndumpsanitizer uses a different approach than svndumpfilter. Svndumpfilter only reads the data once, but the price is that it has to make assumptions about how the repository is constructed. If even one assumption turns out to be incorrect (almost inevitable in bigger repositories) the operation will fail. Svndumpsanitizer on the other hand first reads all the metadata, then analyzes it, then reads the data again copying it sans the parts the user wanted to omit. The price for this approach is that all the data needs to be there when the process is launched.*)
- **Read from stdin.** As explained above, svndumpsanitizer needs to read the data twice. That means it needs the actual dump file, and can not read from stdin or other stream.

*) I know some people have used svndumpsanitizer with partial dumps, and it _might_ work under some circumstances, just be aware that it's neither supported nor recommended. Yes, I know; downtime sucks. Sometimes
there just is no other way, though...

## How it works

It is in fact quite understandable that svndumpfilter doesn't work. It's an aptly named program, because all it does is take a data stream and output the contents to stdout after filtering it on the fly. The problem is that
the subversion repository structure is too complicated for such an approach to even have a theoretical chance of working. When the filter is at revision 10, it has no way of knowing whether a node the user wants to discard, will be moved to a position he wishes to keep in revision 113. So it does the only thing it can do - it discards the node, and at revision 113 craps out because it has already discarded the data it turns out it would have needed.

Svndumpsanitizer works in a different manner. It scans the nodes several times in order to discover which nodes should actually be kept. After it has determined which nodes to keep it writes only these nodes to the
outfile. Finally - if necessary - it adds a commit that deletes any unwanted nodes that had to be kept in order not to break the repository. There are 6 steps in total. (7 if you want to drop empty revisions.)

## Limitations and upcoming features

Version 0.8.0 adds support for dropping and renumbering revisions, so starting then there are no serious known limitations.

It has been pointed out to me that svndumpsanitizer creates a lot of unnecessary newlines in the sanitized files. This is true. If you look at the [changelog](http://miria.homelinuxserver.org/svndumpsanitizer/changelog.txt) you'll see that I tried to address this in version 0.8.2, but eventually got rid of it, because the "fix" intoduced a bug, and the problem is only cosmetic. Svnadmin ignores surplus newlines anyway. If you absolutely must have a clean dump file (instead of "merely" a working one), the workaround is to import the dump and then dump again.

## Bug reports

Bugs can be reported to daniel[dot]suni[at]gmail[dot]com. If the problem is with a specific dumpfile, please include the offending dumpfile. If the contents of the repository is too sensitive/secret/embarrassing/too
freakin' huge to post, then please try to recreate the problem with a simple non-sensitive dumpfile.

You can also try creating a non-sensitive dumpfile by using dumpstrip, a tool that strips out all the data, leaving only the metadata (which is usually the interesting part from a debugging perspective). Dumpstrip is used like this:
```sh
$ dumpstrip --infile foobar.dump --outfile stripped.dump
```
Oh, and if you can code and use gdb, patches are of course welcome. Thanks, Gary (and everyone else). :-)