/*
 *   GStreamer base class for non-streaming audio decoders
 *   Copyright (C) 2013 Carlos Rafael Giani
 *
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */


#ifndef GSTNONSTREAMAUDIODEC_H
#define GSTNONSTREAMAUDIODEC_H

#include <gst/gst.h>
#include <gst/base/gstadapter.h>
#include <gst/audio/audio.h>


G_BEGIN_DECLS


typedef struct _GstNonstreamAudioDecoder GstNonstreamAudioDecoder;
typedef struct _GstNonstreamAudioDecoderClass GstNonstreamAudioDecoderClass;


typedef enum
{
	GST_NONSTREM_AUDIO_OUTPUT_MODE_LOOPING = 0,
	GST_NONSTREM_AUDIO_OUTPUT_MODE_STEADY = 1,
	GST_NONSTREM_AUDIO_OUTPUT_MODE_UNDEFINED
} GstNonstreamAudioOutputMode;


#define GST_TYPE_NONSTREAM_AUDIO_DECODER             (gst_nonstream_audio_decoder_get_type())
#define GST_NONSTREAM_AUDIO_DECODER(obj)             (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_NONSTREAM_AUDIO_DECODER, GstNonstreamAudioDecoder))
#define GST_NONSTREAM_AUDIO_DECODER_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_NONSTREAM_AUDIO_DECODER, GstNonstreamAudioDecoderClass))
#define GST_NONSTREAM_AUDIO_DECODER_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS((obj), GST_TYPE_NONSTREAM_AUDIO_DECODER, GstNonstreamAudioDecoderClass))
#define GST_IS_NONSTREAM_AUDIO_DECODER(obj)          (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_NONSTREAM_AUDIO_DECODER))
#define GST_IS_NONSTREAM_AUDIO_DECODER_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_NONSTREAM_AUDIO_DECODER))

/**
 * GST_NONSTREAM_AUDIO_DECODER_SINK_NAME:
 *
 * The name of the template for the sink pad.
 */
#define GST_NONSTREAM_AUDIO_DECODER_SINK_NAME    "sink"
/**
 * GST_NONSTREAM_AUDIO_DECODER_SRC_NAME:
 *
 * The name of the template for the source pad.
 */

#define GST_NONSTREAM_AUDIO_DECODER_SRC_NAME     "src"

/**
 * GST_NONSTREAM_AUDIO_DECODER_SINK_PAD:
 * @obj: base nonstream audio codec instance
 *
 * Gives the pointer to the sink #GstPad object of the element.
 */
#define GST_NONSTREAM_AUDIO_DECODER_SINK_PAD(obj)        (((GstNonstreamAudioDecoder *) (obj))->sinkpad)
/**
 * GST_NONSTREAM_AUDIO_DECODER_SRC_PAD:
 * @obj: base nonstream audio codec instance
 *
 * Gives the pointer to the source #GstPad object of the element.
 */

#define GST_NONSTREAM_AUDIO_DECODER_SRC_PAD(obj)         (((GstNonstreamAudioDecoder *) (obj))->srcpad)

#define GST_NONSTREAM_AUDIO_DECODER_STREAM_LOCK(dec)   g_rec_mutex_lock(&(GST_NONSTREAM_AUDIO_DECODER(dec)->stream_lock))
#define GST_NONSTREAM_AUDIO_DECODER_STREAM_UNLOCK(dec) g_rec_mutex_unlock(&(GST_NONSTREAM_AUDIO_DECODER(dec)->stream_lock))


/**
 * GstNonstreamAudioDecoder:
 *
 * The opaque #GstNonstreamAudioDecoder data structure.
 */
struct _GstNonstreamAudioDecoder
{
	GstElement element;

	/*< protected >*/

	/* source and sink pads */
	GstPad *sinkpad, *srcpad;

	GstClockTime duration;
	guint64 offset, num_decoded;
	GstSegment cur_segment;

	guint initial_subsong;
	GstToc *toc;

	gboolean loaded;

	GstNonstreamAudioOutputMode output_mode;

	GstAudioInfo audio_info;
	gboolean output_format_changed;
	gboolean discont;

	GstAllocator *allocator;
	GstAllocationParams allocation_params;

	GstAdapter *adapter;
	gint64 upstream_size; /* used in push mode only */

	GRecMutex stream_lock;
};


/**
 * GstAudioDecoderClass:
 * @element_class:              The parent class structure
 * @seek:                       Optional.
 *                              Called when a seek event is received by the parent class.
 *                              The position is given relative to the current subsong.
 *                              Minimum is 0, maximum is the subsong length.
 * @tell:                       Optional.
 *                              Called when a position query is received by the parent class.
 *                              The position that this function returns must be relative to
 *                              the current subsong. Thus, the minimum is 0, and the maximum
 *                              is the subsong length.
 * @load_from_buffer:           Required.
 *                              Loads the media from the given buffer. The entire media is supplied at once,
 *                              so after this call, loading should be finished. This function
 *                              can also make use of a suggested initial subsong and initial
 *                              playback position (but isn't required to). In case it chooses a different starting
 *                              position, the function must pass this position to *initial_position.
 * @set_current_subsong:        Optional.
 *                              Sets the current subsong. This function is allowed to switch to a different
 *                              subsong than the required one, and can optionally make use of the suggested initial
 *                              position. In case it chooses a different starting position, the function must pass
 *                              this position to *initial_position.
 *                              If this function is implemented by the subclass, @get_current_subsong and
 *                              @get_num_subsongs should be implemented as well.
 * @get_current_subsong:        Optional.
 *                              Returns the current subsong.
 *                              If this function is implemented by the subclass,
 *                              @get_num_subsongs should be implemented as well.
 * @get_num_subsongs:           Optional.
 *                              Returns the number of subsongs available.
 *                              The return values 0 and 1 have a similar, but distinct, meaning.
 *                              If this function returns 0, then this decoder does not support subsongs at all.
 *                              @get_current_subsong must then also always return 0. In other words, this function
 *                              either never returns 0, or never returns anything else than 0.
 *                              A return value of 1 means that the media contains either only one or no subsongs
 *                              (the entire song is then considered to be one single subsong). 1 also means that only
 *                              this very media has no or just one subsong, and the decoder itself can
 *                              support multiple subsongs.
 * @get_subsong_duration:       Optional.
 *                              Returns the duration of a subsong. Returns GST_CLOCK_TIME_NONE if duration is unknown.
 * @get_subsong_tags:           Optional.
 *                              Returns tags for a subsong. Returns NULL if there are no tags.
 * @set_num_loops:              Optional.
 *                              Sets the number of loops for playback. If this is called during playback,
 *                              the subclass must set any internal loop counters to zero. A loop value of -1
 *                              means infinite looping; 0 means no looping; and when the num_loops is greater than 0,
 *                              playback should loop exactly num_loops times. If this function is implemented,
 *                              @get_num_loops should be implemented as well. The function can ignore the given values
 *                              and choose another; however, @get_num_loops should return this other value afterwards.
 *                              It is up to the subclass to define where the loop starts and ends. It can mean that only
 *                              a subset at the end or in the middle of a song is repeated, for example.
 * @get_num_loops:              Optional.
 *                              Returns the number of loops for playback.
 * @get_supported_output_modes: Required.
 *                              Returns a bitmask containing the output modes the subclass supports.
 *                              The mask is formed by a bitwise OR combination of integers, which can be calculated
 *                              this way:  1 << GST_NONSTREM_AUDIO_OUTPUT_MODE_<mode> , where mode is either STEADY or LOOPING
 * @set_output_mode:            Optional.
 *                              Sets the output mode the subclass has to use. Unlike with most other functions, the subclass
 *                              cannot choose a different mode; it must use the requested one.
 *                              If the output mode is set to LOOPING, @gst_nonstream_audio_decoder_handle_loop
 *                              must be called after playback moved back to the start of a loop.
 * @decode:                     Required.
 *                              Allocates an output buffer, fills it with decoded audio samples, and must be passed on to
 *                              *buffer . The number of decoded samples must be passed on to *num_samples.
 *                              If decoding finishes or the decoding is no longer possible (for example, due to an
 *                              unrecoverable error), this function returns FALSE, otherwise TRUE.
 * @decide_allocation:          Optional.
 *                              Sets up the allocation parameters for allocating output
 *                              buffers. The passed in query contains the result of the
 *                              downstream allocation query.
 *                              Subclasses should chain up to the parent implementation to
 *                              invoke the default handler.
 * @propose_allocation:         Optional.
 *                              Proposes buffer allocation parameters for upstream elements.
 *                              Subclasses should chain up to the parent implementation to
 *                              invoke the default handler.
 *
 * Subclasses can override any of the available optional virtual methods or not, as
 * needed. At minimum, @load_from_buffer, @get_supported_output_modes, and @decode need
 * to be overridden.
 *
 * These functions (with the obvious exception of @load_from_buffer) only need to be functional
 * after the media was loaded, since the base class will not call them before.
 */
struct _GstNonstreamAudioDecoderClass
{
	GstElementClass element_class;

	/*< public >*/
	/* virtual methods for subclasses */

	gboolean (*seek)(GstNonstreamAudioDecoder *dec, GstClockTime new_position);
	GstClockTime (*tell)(GstNonstreamAudioDecoder *dec);

	gboolean (*load_from_buffer)(GstNonstreamAudioDecoder *dec, GstBuffer *source_data, guint initial_subsong, GstClockTime *initial_position, GstNonstreamAudioOutputMode *initial_output_mode);

	gboolean (*set_current_subsong)(GstNonstreamAudioDecoder *dec, guint subsong, GstClockTime *initial_position);
	guint (*get_current_subsong)(GstNonstreamAudioDecoder *dec);

	guint (*get_num_subsongs)(GstNonstreamAudioDecoder *dec);
	GstClockTime (*get_subsong_duration)(GstNonstreamAudioDecoder *dec, guint subsong);
	GstTagList* (*get_subsong_tags)(GstNonstreamAudioDecoder *dec, guint subsong);

	gboolean (*set_num_loops)(GstNonstreamAudioDecoder *dec, gint num_loops);
	gint (*get_num_loops)(GstNonstreamAudioDecoder *dec);

	guint (*get_supported_output_modes)(GstNonstreamAudioDecoder *dec);
	gboolean (*set_output_mode)(GstNonstreamAudioDecoder *dec, GstNonstreamAudioOutputMode mode, GstClockTime *current_position);

	gboolean (*decode)(GstNonstreamAudioDecoder *dec, GstBuffer **buffer, guint *num_samples);

	gboolean (*negotiate)(GstNonstreamAudioDecoder *dec);

	gboolean (*decide_allocation)(GstNonstreamAudioDecoder *dec, GstQuery *query);
	gboolean (*propose_allocation)(GstNonstreamAudioDecoder *dec, GstQuery * query);
};


GType gst_nonstream_audio_decoder_get_type(void);


void gst_nonstream_audio_decoder_handle_loop(GstNonstreamAudioDecoder *dec, GstClockTime new_position);

gboolean gst_nonstream_audio_decoder_set_output_audioinfo(GstNonstreamAudioDecoder *dec, GstAudioInfo const *info);
gboolean gst_nonstream_audio_decoder_negotiate(GstNonstreamAudioDecoder *dec);
void gst_nonstream_audio_decoder_get_downstream_info(GstNonstreamAudioDecoder *dec, GstAudioFormat *format, gint *sample_rate, gint *num_channels);

GstBuffer * gst_nonstream_audio_decoder_allocate_output_buffer(GstNonstreamAudioDecoder *dec, gsize size);


G_END_DECLS


#endif

