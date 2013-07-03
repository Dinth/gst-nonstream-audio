gst-nonstream-audio
===================

About
-----

This is a set of plugins for [GStreamer 1.0](http://gstreamer.freedesktop.org/) audio decoders
which are not based on a streaming model. These decoders load the encoded data completely, at
once, and then commence playback. Examples are: module music (MOD, S3M, XM, IT, ...) , video
game music (VGM, GYM, GBS, ...) various Amiga music files, AdLib music, etc.

GStreamer's [GstAudioDecoder](http://gstreamer.freedesktop.org/data/doc/gstreamer/head/gst-plugins-base-libs/html/gst-plugins-base-libs-gstaudiodecoder.html)
base class is very powerful, and suitable for most kinds of audio decoders. However, it assumes
a certain kind of model that is simply not applicable for the aforementioned types of media.
Using that base class for this purpose ends up in massive amounts of overrides, effectively
creating a new base class. So it was decided to do just that - create a new bass class, called
`GstNonstreamAudioDecoder`. It takes care of most GStreamer specifics and adds support for
subsongs (essentially like alternate tracks in the same song) and loops (songs can contain loops,
which cause playback to go back to the beginning, or playback loops in a subset of the song).


**PLEASE NOTE THAT THIS CODE IS STILL IN AN ALPHA STAGE! Compatibility-breaking changes may happen!**


Available plugins
-----------------

* dumbdec : module music decoder using the DUMB (Dynamic Universal Music Bibliotheque) library.
  Instead of the [original](http://dumb.sf.net/), which has been abandoned since 2005, the plugin
  uses kode54's [improved version](https://github.com/kode54/dumb). Note that the plugin will
  not work correctly with the original; however, this codebase contains the improved version as a
  git submodule.
* gmedec : video game music decoder using the Game Music Emulator library. As with DUMB, an
  improved version is used. However, even though it works just fine, it is currently disabled
  because it requires some fixes to the GME fork (which, like DUMB, is contained as a git submodule).
* openmpt : module music decoder using libopenmpt, a library version of [OpenMPT](http://openmpt.org/).
  libopenmpt is part of an extensive OpenMPT refactoring, and thus has an unstable API at the moment.
  For this reason, this plugin is disabled by default (works fine though).


How to build
------------

Right after git-cloning this repository, run:

    git submodule init
    git submodule update

Then, execute this line:

    ./waf configure

or this one if you want to have the plugins installed somewhere else (default is $PREFIX/lib/gstreamer-1.0):

    ./waf configure --plugin-install-path=<path/to/target/location>

GStreamer 1.0 typically uses `$HOME/.local/share/gstreamer-1.0/plugins/ ` for local plugins.
To build and install after configuring, simply run:

    ./waf
    ./waf install

Now there should be a `dumbdec` plugin. To check, run:

    gst-inspect-1.0 dumbdec

**NOTE:** currently, there may be linking problems if more than plugin is installed and in GStreamer's registry
at the same time. The reason for this is that GstNonstreamAudioDecoder is inside a static library, which causes
class collisions between plugins. This will be fixed soon. You have been warned about this software's alpha status...!

