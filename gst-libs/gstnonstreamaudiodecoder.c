#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <gst/base/gstadapter.h>

#include "gstnonstreamaudiodecoder.h"


GST_DEBUG_CATEGORY (nonstream_audiodecoder_debug);
#define GST_CAT_DEFAULT nonstream_audiodecoder_debug


enum
{
	PROP_0,
	PROP_CURRENT_SUBSONG,
	PROP_NUM_SUBSONGS
};

#define DEFAULT_CURRENT_SUBSONG 0
#define DEFAULT_NUM_SUBSONGS 0


static void gst_nonstream_audio_decoder_class_init(GstNonstreamAudioDecoderClass *klass);
static void gst_nonstream_audio_decoder_init(GstNonstreamAudioDecoder *dec, GstNonstreamAudioDecoderClass *klass);

static GstStateChangeReturn gst_nonstream_audio_decoder_change_state(GstElement *element, GstStateChange transition);


static gboolean gst_nonstream_audio_decoder_sink_event(GstPad *pad, GstObject *parent, GstEvent *event);
static gboolean gst_nonstream_audio_decoder_src_event(GstPad *pad, GstObject *parent, GstEvent *event);
static gboolean gst_nonstream_audio_decoder_src_query(GstPad *pad, GstObject *parent, GstQuery *query);

static gboolean gst_nonstream_audio_decoder_do_seek(GstNonstreamAudioDecoder *dec, GstEvent *event);

static gboolean gst_nonstream_audio_decoder_sinkpad_activate(GstPad *pad, GstObject *parent);
static gboolean gst_nonstream_audio_decoder_sinkpad_activate_mode(GstPad *pad, GstObject *parent, GstPadMode mode, gboolean active);

static gboolean gst_nonstream_audio_decoder_get_upstream_size(GstNonstreamAudioDecoder *dec, gint64 *length);
static void gst_nonstream_audio_decoder_loop(GstNonstreamAudioDecoder *dec);

static gboolean gst_nonstream_audio_decoder_negotiate_default(GstNonstreamAudioDecoder *dec);
static gboolean gst_nonstream_audio_decoder_decide_allocation_default(GstNonstreamAudioDecoder *dec, GstQuery *query);
static gboolean gst_nonstream_audio_decoder_propose_allocation_default(GstNonstreamAudioDecoder *dec, GstQuery *query);

static void gst_nonstream_audio_decoder_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void gst_nonstream_audio_decoder_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);

static void gst_nonstream_audio_decoder_finalize(GObject *object);

static GstElementClass *gst_nonstream_audio_decoder_parent_class = NULL;



/*
TODO:
- hard-flush & reset functions (if these make sense with module etc. music)
- offset vs. tell()
- study GstAudioDecoder code more
- what about negative playback rates ?
- loops
*/



static char const * get_seek_type_name(GstSeekType seek_type)
{
	switch (seek_type)
	{
		case GST_SEEK_TYPE_NONE: return "none";
		case GST_SEEK_TYPE_SET: return "set";
		case GST_SEEK_TYPE_END: return "end";
		default: return "<unknown>";
	}
}



GType gst_nonstream_audio_decoder_get_type(void)
{
	static volatile gsize nonstream_audio_decoder_type = 0;

	if (g_once_init_enter(&nonstream_audio_decoder_type))
	{
		GType type_;
		static const GTypeInfo nonstream_audio_decoder_info =
		{
			sizeof(GstNonstreamAudioDecoderClass),
			NULL,
			NULL,
			(GClassInitFunc)gst_nonstream_audio_decoder_class_init,
			NULL,
			NULL,
			sizeof(GstNonstreamAudioDecoder),
			0,
			(GInstanceInitFunc)gst_nonstream_audio_decoder_init,
			NULL /* TODO: correct? */
		};

		type_ = g_type_register_static(
			GST_TYPE_ELEMENT,
			"GstNonstreamAudioDecoder",
			&nonstream_audio_decoder_info,
			G_TYPE_FLAG_ABSTRACT
		);
		g_once_init_leave(&nonstream_audio_decoder_type, type_);
	}

	return nonstream_audio_decoder_type;
}


static void gst_nonstream_audio_decoder_class_init(GstNonstreamAudioDecoderClass *klass)
{
	GObjectClass *object_class;
	GstElementClass *element_class;
	GstNonstreamAudioDecoderClass *dec_class;

	object_class = G_OBJECT_CLASS(klass);
	element_class = GST_ELEMENT_CLASS(klass);
	dec_class = GST_NONSTREAM_AUDIO_DECODER_CLASS(klass);

	gst_nonstream_audio_decoder_parent_class = g_type_class_peek_parent(klass);

	GST_DEBUG_CATEGORY_INIT(nonstream_audiodecoder_debug, "nonstreamaudiodecoder", 0, "nonstream audio decoder base class");

	object_class->finalize = GST_DEBUG_FUNCPTR(gst_nonstream_audio_decoder_finalize);
	element_class->change_state = GST_DEBUG_FUNCPTR(gst_nonstream_audio_decoder_change_state);
	object_class->set_property = GST_DEBUG_FUNCPTR(gst_nonstream_audio_decoder_set_property);
	object_class->get_property = GST_DEBUG_FUNCPTR(gst_nonstream_audio_decoder_get_property);

	dec_class->seek = NULL;
	dec_class->tell = NULL;
	dec_class->load = NULL;
	dec_class->get_current_subsong = NULL;
	dec_class->set_current_subsong = NULL;
	dec_class->decode = NULL;
	dec_class->negotiate = GST_DEBUG_FUNCPTR(gst_nonstream_audio_decoder_negotiate_default);
	dec_class->decide_allocation = GST_DEBUG_FUNCPTR(gst_nonstream_audio_decoder_decide_allocation_default);
	dec_class->propose_allocation = GST_DEBUG_FUNCPTR(gst_nonstream_audio_decoder_propose_allocation_default);
}


static void gst_nonstream_audio_decoder_init(GstNonstreamAudioDecoder *dec, GstNonstreamAudioDecoderClass *klass)
{
	GstPadTemplate *pad_template;

	dec->duration = GST_CLOCK_TIME_NONE;
	dec->offset = 0;
	dec->num_subsongs = DEFAULT_NUM_SUBSONGS;
	dec->loaded = FALSE;
	dec->allocator = NULL;

	g_rec_mutex_init(&(dec->stream_lock));

	gst_audio_info_init(&(dec->audio_info));

	pad_template = gst_element_class_get_pad_template(GST_ELEMENT_CLASS(klass), "sink");
	g_return_if_fail(pad_template != NULL);
	dec->sinkpad = gst_pad_new_from_template(pad_template, "sink");
	gst_pad_set_event_function(dec->sinkpad, GST_DEBUG_FUNCPTR(gst_nonstream_audio_decoder_sink_event));
	gst_pad_set_activate_function(dec->sinkpad, GST_DEBUG_FUNCPTR(gst_nonstream_audio_decoder_sinkpad_activate));
	gst_pad_set_activatemode_function(dec->sinkpad, GST_DEBUG_FUNCPTR(gst_nonstream_audio_decoder_sinkpad_activate_mode));
	gst_element_add_pad(GST_ELEMENT(dec), dec->sinkpad);

	pad_template = gst_element_class_get_pad_template(GST_ELEMENT_CLASS(klass), "src");
	g_return_if_fail(pad_template != NULL);
	dec->srcpad = gst_pad_new_from_template(pad_template, "src");
	gst_pad_set_event_function(dec->srcpad, GST_DEBUG_FUNCPTR(gst_nonstream_audio_decoder_src_event));
	gst_pad_set_query_function(dec->srcpad, GST_DEBUG_FUNCPTR(gst_nonstream_audio_decoder_src_query));
	gst_pad_use_fixed_caps(dec->srcpad);	
	gst_element_add_pad(GST_ELEMENT(dec), dec->srcpad);
}


static GstStateChangeReturn gst_nonstream_audio_decoder_change_state(GstElement *element, GstStateChange transition)
{
	GstStateChangeReturn ret;

	ret = GST_ELEMENT_CLASS(gst_nonstream_audio_decoder_parent_class)->change_state(element, transition);

	switch (transition)
	{
		case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
			break;
		case GST_STATE_CHANGE_PAUSED_TO_READY:
			break;
		case GST_STATE_CHANGE_READY_TO_NULL:
			break;
		default:
			break;
	}

	return ret;
}


static gboolean gst_nonstream_audio_decoder_sink_event(GstPad *pad, GstObject *parent, GstEvent *event)
{
	switch(GST_EVENT_TYPE (event))
	{
		default:
			return gst_pad_event_default(pad, parent, event);
	}
}


static gboolean gst_nonstream_audio_decoder_src_event(GstPad *pad, GstObject *parent, GstEvent *event)
{
	gboolean res;
	GstNonstreamAudioDecoder *dec;
	GstNonstreamAudioDecoderClass *dec_class;

	res = FALSE;
	dec = GST_NONSTREAM_AUDIO_DECODER(parent);

	switch (GST_EVENT_TYPE(event))
	{
		case GST_EVENT_SEEK:
			dec_class = GST_NONSTREAM_AUDIO_DECODER_GET_CLASS(dec);
			if (dec_class->seek != NULL)
				res = gst_nonstream_audio_decoder_do_seek(dec, event);
			break;
		default:
			break;
	}

	if (!res)
		res = gst_pad_event_default(pad, parent, event);

	return res;
}


static gboolean gst_nonstream_audio_decoder_src_query(GstPad *pad, GstObject *parent, GstQuery *query)
{
	gboolean res;
	GstNonstreamAudioDecoder *dec;
	GstNonstreamAudioDecoderClass *dec_class;
	GstFormat format;

	res = FALSE;
	dec = GST_NONSTREAM_AUDIO_DECODER(parent);
	dec_class = GST_NONSTREAM_AUDIO_DECODER_GET_CLASS(dec);

	switch (GST_QUERY_TYPE(query))
	{
		case GST_QUERY_DURATION:
		{
			if (!dec->loaded)
			{
				GST_DEBUG_OBJECT(parent, "cannot respond to duration query: nothing is loaded yet");
				break;
			}

			gst_query_parse_duration(query, &format, NULL);
			if ((format == GST_FORMAT_TIME) && (dec->duration != GST_CLOCK_TIME_NONE))
			{
				gst_query_set_duration(query, format, dec->duration);
				res = TRUE;
			}

			break;
		}
		case GST_QUERY_POSITION:
		{
			GST_DEBUG_OBJECT(parent, "position query received");

			if (!dec->loaded)
			{
				GST_DEBUG_OBJECT(parent, "cannot respond to position query: nothing is loaded yet");
				break;
			}

			if (dec_class->tell == NULL)
			{
				GST_DEBUG_OBJECT(parent, "cannot respond to position query: subclass does not have tell() function defined");
				break;
			}

			gst_query_parse_position(query, &format, NULL);
			if (format == GST_FORMAT_TIME)
			{
				GstClockTime pos = dec_class->tell(dec);
				GST_DEBUG_OBJECT(parent, "position query received with format TIME -> reporting position %" GST_TIME_FORMAT, GST_TIME_ARGS(pos));
        			gst_query_set_position(query, format, pos);
				res = TRUE;
			}
			else
			{
				GST_DEBUG_OBJECT(parent, "position query received with unsupported format %s -> not reporting anything", gst_format_get_name(format));
			}

			break;
		}
		case GST_QUERY_SEEKING:
		{
			GstFormat fmt;

			if (!dec->loaded)
			{
				GST_DEBUG_OBJECT(parent, "cannot respond to position query: nothing is loaded yet");
				break;
			}

			if (dec_class->seek == NULL)
			{
				GST_DEBUG_OBJECT(parent, "cannot respond to seeking query: subclass does not have seek() function defined");
				break;
			}

			gst_query_parse_seeking(query, &fmt, NULL, NULL, NULL);

			if (fmt == GST_FORMAT_TIME)
			{
				GST_DEBUG_OBJECT(parent, "seeking query received with format TIME -> can seek: yes");
				gst_query_set_seeking(query, GST_FORMAT_TIME, TRUE, dec->cur_segment.start, dec->cur_segment.stop);
				res = TRUE;
			}
			else
				GST_DEBUG_OBJECT(parent, "seeking query received with unsupported format %s -> can seek: no", gst_format_get_name(format));

			break;
		}
		default:
			break;
	}

	if (!res)
		res = gst_pad_query_default(pad, parent, query);

	return res;
}


static gboolean gst_nonstream_audio_decoder_do_seek(GstNonstreamAudioDecoder *dec, GstEvent *event)
{
	gboolean res;
	gdouble rate;
	GstFormat format;
	GstSeekFlags flags;
	GstSeekType start_type, stop_type;
	gint64 start, stop;
	GstSegment segment;
	gboolean flush;
	GstNonstreamAudioDecoderClass *dec_class;

	if (!dec->loaded)
	{
		GST_DEBUG_OBJECT(dec, "nothing loaded yet - cannot seek");
		return FALSE;
	}
	if (!GST_AUDIO_INFO_IS_VALID(&(dec->audio_info)))
	{
		GST_DEBUG_OBJECT(dec, "no valid audioinfo present - cannot seek");
		return FALSE;
	}

	GST_DEBUG_OBJECT(dec, "starting seek");

	gst_event_parse_seek(event, &rate, &format, &flags, &start_type, &start, &stop_type, &stop);

	GST_DEBUG_OBJECT(
		dec,
		"seek event data:  "
		"rate %f  format %s  "
		"start type %s  start %" GST_TIME_FORMAT "  "
		"stop type %s  stop %" GST_TIME_FORMAT,
		rate, gst_format_get_name(format),
		get_seek_type_name(start_type), GST_TIME_ARGS(start),
		get_seek_type_name(stop_type), GST_TIME_ARGS(stop)
	);

	if (format != GST_FORMAT_TIME)
	{
		GST_DEBUG_OBJECT(dec, "seeking is only supported in TIME format");
		return FALSE;
	}

	flush = ((flags & GST_SEEK_FLAG_FLUSH) == GST_SEEK_FLAG_FLUSH);

	if (flush)
	{
		gst_pad_push_event(dec->srcpad, gst_event_new_flush_start());
	        /* unlock upstream pull_range */
	        gst_pad_push_event(dec->sinkpad, gst_event_new_flush_start());
	}
	else
		gst_pad_pause_task(dec->sinkpad);

	GST_PAD_STREAM_LOCK (dec->sinkpad);

	segment = dec->cur_segment;

	if (!gst_segment_do_seek(
		&segment,
		rate,
		format,
		flags,
		start_type,
		start,
		stop_type,
		stop,
		NULL
	))
	{
    		GST_DEBUG_OBJECT(dec, "could not seek in segment");
		GST_PAD_STREAM_UNLOCK(dec->sinkpad);
		return FALSE;
	}

	if ((stop == -1) && (dec->duration > 0))
		stop = dec->duration;

	GST_DEBUG_OBJECT(
		dec,
		"segment data: "
		"seek event data:  "
		"rate %f  applied rate %f  "
		"format %s  "
		"base %" GST_TIME_FORMAT "  "
		"offset %" GST_TIME_FORMAT "  "
		"start %" GST_TIME_FORMAT "  "
		"stop %" GST_TIME_FORMAT "  "
		"time %" GST_TIME_FORMAT "  "
		"position %" GST_TIME_FORMAT "  "
		"duration %" GST_TIME_FORMAT,
		segment.rate, segment.applied_rate,
		gst_format_get_name(segment.format),
		GST_TIME_ARGS(segment.base),
		GST_TIME_ARGS(segment.offset),
		GST_TIME_ARGS(segment.start),
		GST_TIME_ARGS(segment.stop),
		GST_TIME_ARGS(segment.time),
		GST_TIME_ARGS(segment.position),
		GST_TIME_ARGS(segment.duration)
	);

	dec_class = GST_NONSTREAM_AUDIO_DECODER_GET_CLASS(dec);

	res = dec_class->seek(dec, segment.position);

	if (res)
	{
		dec->cur_segment = segment;
		dec->offset = gst_util_uint64_scale_int(dec->cur_segment.position, dec->audio_info.rate, GST_SECOND);

		if (flags & GST_SEEK_FLAG_SEGMENT)
		{
			GST_DEBUG_OBJECT (dec, "posting SEGMENT_START message");

			gst_element_post_message(
				GST_ELEMENT(dec),
				gst_message_new_segment_start(
					GST_OBJECT(dec),
					GST_FORMAT_TIME, segment.start
				)
			);
		}
		if (flush)
		{
			gst_pad_push_event(dec->srcpad, gst_event_new_flush_stop(TRUE));
			gst_pad_push_event(dec->sinkpad, gst_event_new_flush_stop(TRUE));
		}

		gst_pad_push_event(dec->srcpad, gst_event_new_segment(&segment));

		GST_INFO_OBJECT(dec, "seek succeeded");

		gst_pad_start_task(dec->sinkpad, (GstTaskFunction)gst_nonstream_audio_decoder_loop, dec, NULL);
	}
	else
	{
		GST_WARNING_OBJECT(dec, "seek failed");
	}


	GST_PAD_STREAM_UNLOCK(dec->sinkpad);
	gst_event_unref(event);

	return res;
}


static gboolean gst_nonstream_audio_decoder_sinkpad_activate(GstPad *pad, G_GNUC_UNUSED GstObject *parent)
{
	GstQuery *query;
	gboolean do_activate_pull;

	query = gst_query_new_scheduling();

	if (gst_pad_peer_query(pad, query))
		do_activate_pull = gst_query_has_scheduling_mode_with_flags(query, GST_PAD_MODE_PULL, GST_SCHEDULING_FLAG_SEEKABLE);
	else
		do_activate_pull = FALSE;

	gst_query_unref(query);

	GST_DEBUG_OBJECT(pad, "activating %s", do_activate_pull ? "pull" : "push");
	return gst_pad_activate_mode(pad, do_activate_pull ? GST_PAD_MODE_PULL : GST_PAD_MODE_PUSH, TRUE);
}


static gboolean gst_nonstream_audio_decoder_sinkpad_activate_mode(GstPad *pad, GstObject *parent, GstPadMode mode, gboolean active)
{
	gboolean res;

	switch (mode)
	{
		/* TODO: what if pull mode is not possible? */
		case GST_PAD_MODE_PUSH:
			res = TRUE;
			break;
		case GST_PAD_MODE_PULL:
			if (active)
				res = gst_pad_start_task(pad, (GstTaskFunction)gst_nonstream_audio_decoder_loop, parent, NULL);
			else
				res = gst_pad_stop_task(pad);
			break;
		default:
			res = FALSE;
			break;
	}

	return res;
}


static gboolean gst_nonstream_audio_decoder_get_upstream_size(GstNonstreamAudioDecoder *dec, gint64 *length)
{
	GstPad *peer;
	gboolean res = FALSE;

	peer = gst_pad_get_peer(dec->sinkpad);
	if (peer == NULL)
		return FALSE;

	res = (gst_pad_query_duration(peer, GST_FORMAT_BYTES, length) && ((*length) >= 0));

	gst_object_unref(peer);

	return res;
}


static void gst_nonstream_audio_decoder_loop(GstNonstreamAudioDecoder *dec)
{
	GstNonstreamAudioDecoderClass *dec_class;
	dec_class = GST_NONSTREAM_AUDIO_DECODER_CLASS(G_OBJECT_GET_CLASS(dec));

	if (!dec->loaded)
	{
		gint64 size;
		GstBuffer *buffer;
		GstFlowReturn flow;
		gboolean module_ok;

		g_assert(dec_class->load != NULL);

		if (!gst_nonstream_audio_decoder_get_upstream_size(dec, &size))
		{
			GST_ELEMENT_ERROR(dec, STREAM, DECODE, (NULL), ("Cannot load - upstream size (in bytes) cannot be determined"));
			goto pause;
		}

		buffer = NULL;
		flow = gst_pad_pull_range(dec->sinkpad, 0, size, &buffer);
		if (flow != GST_FLOW_OK)
		{
			GST_ELEMENT_ERROR(dec, STREAM, DECODE, (NULL), ("Cannot load - pulling data from upstream failed (flow error: %s)", gst_flow_get_name(flow)));
			goto pause;
		}

		GST_LOG_OBJECT(dec, "Read %u bytes from upstream", gst_buffer_get_size(buffer));

		module_ok = dec_class->load(dec, buffer);
		gst_buffer_unref(buffer);

		if (!module_ok)
		{
			GST_ELEMENT_ERROR(dec, STREAM, DECODE, (NULL), ("Loading failed"));
			goto pause;
		}

		dec->loaded = TRUE;

		gst_segment_init(&(dec->cur_segment), GST_FORMAT_TIME);
		dec->cur_segment.stop = dec->duration;
		dec->cur_segment.duration = dec->duration;
		gst_pad_push_event(dec->srcpad, gst_event_new_segment(&(dec->cur_segment)));
	}

	/* loading is done at this point -> send samples downstream */
	{
		GstFlowReturn flow;
		GstBuffer *outbuf;
		guint num_samples;

		g_assert(dec_class->decode != NULL);

		GST_NONSTREAM_AUDIO_DECODER_STREAM_LOCK(dec);

		if (dec_class->decode(dec, &outbuf, &num_samples))
		{
			if (G_UNLIKELY(
				dec->output_format_changed ||
				(GST_AUDIO_INFO_IS_VALID(&(dec->audio_info)) && gst_pad_check_reconfigure(dec->srcpad))
			))
			{
				if (!gst_nonstream_audio_decoder_negotiate(dec))
				{
					gst_buffer_unref(outbuf);
					GST_LOG_OBJECT(dec, "could not push output buffer: negotiation failed");
					goto pause_unlock;
				}
			}

			GST_BUFFER_DURATION(outbuf)  = gst_util_uint64_scale_int(num_samples, GST_SECOND, dec->audio_info.rate);
			GST_BUFFER_OFFSET(outbuf)    = dec->offset;
			GST_BUFFER_TIMESTAMP(outbuf) = gst_util_uint64_scale_int(dec->offset, GST_SECOND, dec->audio_info.rate);

			dec->offset += num_samples;

			flow = gst_pad_push(dec->srcpad, outbuf);

			if (flow != GST_FLOW_OK)
			{
				/* no need to unref buffer - gst_pad_push() does it in all cases (success and failure) */
				GST_LOG_OBJECT(dec, "flow error when pushing output buffer: %s", gst_flow_get_name(flow));
				goto pause_unlock;
			}
		}
		else
		{
			GST_INFO_OBJECT(dec, "decode() returned NULL buffer -> sending EOS event");
			gst_pad_push_event(dec->srcpad, gst_event_new_eos());
			goto pause;
		}

		GST_NONSTREAM_AUDIO_DECODER_STREAM_UNLOCK(dec);
	}

	return;

pause:
	GST_INFO_OBJECT(dec, "pausing task");
	/* NOT using stop_task here, since that would cause a deadlock */
	gst_pad_pause_task(dec->sinkpad);
	return;
pause_unlock:
	GST_NONSTREAM_AUDIO_DECODER_STREAM_UNLOCK(dec);
	goto pause;
}


static gboolean gst_nonstream_audio_decoder_negotiate_default(GstNonstreamAudioDecoder *dec)
{
	GstCaps *caps;
	GstNonstreamAudioDecoderClass *dec_class;
	gboolean res = TRUE;
	GstQuery *query = NULL;
	GstAllocator *allocator;
	GstAllocationParams allocation_params;

	g_return_val_if_fail(GST_IS_NONSTREAM_AUDIO_DECODER(dec), FALSE);
	g_return_val_if_fail(GST_AUDIO_INFO_IS_VALID(&(dec->audio_info)), FALSE);

	dec_class = GST_NONSTREAM_AUDIO_DECODER_CLASS(G_OBJECT_GET_CLASS(dec));

	caps = gst_audio_info_to_caps(&(dec->audio_info));

	GST_DEBUG_OBJECT(dec, "setting src caps %" GST_PTR_FORMAT, caps);

	res = gst_pad_set_caps(dec->srcpad, caps);

	if (!res)
		goto done;

	dec->output_format_changed = FALSE;

	query = gst_query_new_allocation(caps, TRUE);
	if (!gst_pad_peer_query(dec->srcpad, query))
	{
		GST_DEBUG_OBJECT (dec, "didn't get downstream ALLOCATION hints");
	}

	g_assert(dec_class->decide_allocation != NULL);
	res = dec_class->decide_allocation (dec, query);

	GST_DEBUG_OBJECT(dec, "ALLOCATION (%d) params: %" GST_PTR_FORMAT, res, query);

	if (!res)
		goto no_decide_allocation;

	/* we got configuration from our peer or the decide_allocation method,
	 * parse them */
	if (gst_query_get_n_allocation_params(query) > 0)
	{
		gst_query_parse_nth_allocation_param(query, 0, &allocator, &allocation_params);
	}
	else
	{
		allocator = NULL;
		gst_allocation_params_init(&allocation_params);
	}

	if (dec->allocator != NULL)
		gst_object_unref(dec->allocator);
	dec->allocator = allocator;
	dec->allocation_params = allocation_params;

done:
	if (query != NULL)
		gst_query_unref(query);
	gst_caps_unref(caps);

	return res;

no_decide_allocation:
	{
		GST_WARNING_OBJECT(dec, "subclass failed to decide allocation");
		goto done;
	}
}


static gboolean gst_nonstream_audio_decoder_decide_allocation_default(G_GNUC_UNUSED GstNonstreamAudioDecoder *dec, GstQuery *query)
{
	GstAllocator *allocator = NULL;
	GstAllocationParams params;
	gboolean update_allocator;

	/* we got configuration from our peer or the decide_allocation method,
	 * parse them */
	if (gst_query_get_n_allocation_params(query) > 0)
	{
		/* try the allocator */
		gst_query_parse_nth_allocation_param(query, 0, &allocator, &params);
		update_allocator = TRUE;
	}
	else
	{
		allocator = NULL;
		gst_allocation_params_init(&params);
		update_allocator = FALSE;
	}

	if (update_allocator)
		gst_query_set_nth_allocation_param(query, 0, allocator, &params);
	else
		gst_query_add_allocation_param(query, allocator, &params);
	if (allocator)
		gst_object_unref(allocator);

	return TRUE;
}


static gboolean gst_nonstream_audio_decoder_propose_allocation_default(G_GNUC_UNUSED GstNonstreamAudioDecoder *dec, G_GNUC_UNUSED GstQuery *query)
{
	return TRUE;
}


static void gst_nonstream_audio_decoder_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	GstNonstreamAudioDecoder *dec = GST_NONSTREAM_AUDIO_DECODER(object);

	switch (prop_id)
	{
		case PROP_CURRENT_SUBSONG:
		{
			guint new_subsong = g_value_get_uint(value);
			GST_NONSTREAM_AUDIO_DECODER_STREAM_LOCK(dec);
			if (dec->num_subsongs == 0)
				GST_INFO_OBJECT(dec, "ignoring request to set current subsong to %u, since num_subsongs == 0 (= subsongs not supported)", new_subsong);
			else if (new_subsong >= dec->num_subsongs)
				GST_WARNING_OBJECT(dec, "ignoring request to set current subsong to %u, since %u < num_subsongs (%u)", new_subsong, new_subsong, dec->num_subsongs);
			else
			{
				GstNonstreamAudioDecoderClass *klass = GST_NONSTREAM_AUDIO_DECODER_GET_CLASS(dec);
				if (klass->set_current_subsong != NULL)
				{
					if (!(klass->set_current_subsong(dec, new_subsong)))
						GST_WARNING_OBJECT(dec, "switching to new subsong %u failed", new_subsong);
				}
				else
					GST_INFO_OBJECT(dec, "cannot set current subsong - set_current_subsong is NULL");
			}
			GST_NONSTREAM_AUDIO_DECODER_STREAM_UNLOCK(dec);
			break;
		}
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	}
}


static void gst_nonstream_audio_decoder_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	GstNonstreamAudioDecoder *dec = GST_NONSTREAM_AUDIO_DECODER(object);

	switch (prop_id)
	{
		case PROP_CURRENT_SUBSONG:
		{
			GstNonstreamAudioDecoderClass *klass = GST_NONSTREAM_AUDIO_DECODER_GET_CLASS(dec);
			GST_NONSTREAM_AUDIO_DECODER_STREAM_LOCK(dec);
			if (klass->get_current_subsong != NULL)
				g_value_set_uint(value, klass->get_current_subsong(dec));
			else
				GST_INFO_OBJECT(dec, "cannot get current subsong - get_current_subsong is NULL");
			GST_NONSTREAM_AUDIO_DECODER_STREAM_UNLOCK(dec);
			break;
		}
		case PROP_NUM_SUBSONGS:
		{
			GST_NONSTREAM_AUDIO_DECODER_STREAM_LOCK(dec);
			g_value_set_uint(value, dec->num_subsongs);
			GST_NONSTREAM_AUDIO_DECODER_STREAM_UNLOCK(dec);
			break;
		}
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	}
}


static void gst_nonstream_audio_decoder_finalize(GObject *object)
{
	GstNonstreamAudioDecoder *dec;
	
	g_return_val_if_fail(GST_IS_NONSTREAM_AUDIO_DECODER(object), FALSE);
	dec = GST_NONSTREAM_AUDIO_DECODER(object);

	g_rec_mutex_clear(&(dec->stream_lock));

	G_OBJECT_CLASS(gst_nonstream_audio_decoder_parent_class)->finalize(object);
}


void gst_nonstream_audio_decoder_set_duration(GstNonstreamAudioDecoder *dec, GstClockTime duration)
{
	g_return_if_fail(GST_IS_NONSTREAM_AUDIO_DECODER(dec));
	dec->duration = duration;
}


void gst_nonstream_audio_decoder_init_subsong_properties(GstNonstreamAudioDecoderClass *klass)
{
	GObjectClass *object_class;

	g_return_if_fail(GST_IS_NONSTREAM_AUDIO_DECODER_CLASS(klass));

	object_class = G_OBJECT_CLASS(klass);

	g_object_class_install_property(
		object_class,
		PROP_CURRENT_SUBSONG,
		g_param_spec_uint(
			"current-subsong",
			"Currently active subsong",
			"Subsong that is currently selected for playback",
			0, G_MAXUINT,
			DEFAULT_CURRENT_SUBSONG,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);
	g_object_class_install_property(
		object_class,
		PROP_NUM_SUBSONGS,
		g_param_spec_uint(
			"num-subsongs",
			"Number of available subsongs",
			"Subsongs available for playback (0 = media does not support subsongs)",
			0, G_MAXUINT,
			DEFAULT_NUM_SUBSONGS,
			G_PARAM_READABLE | G_PARAM_STATIC_STRINGS
		)
	);
}


void gst_nonstream_audio_decoder_set_num_subsongs(GstNonstreamAudioDecoder *dec, guint num_subsongs)
{
	g_return_if_fail(GST_IS_NONSTREAM_AUDIO_DECODER(dec));

	GST_NONSTREAM_AUDIO_DECODER_STREAM_LOCK(dec);
	dec->num_subsongs = num_subsongs;
	GST_NONSTREAM_AUDIO_DECODER_STREAM_UNLOCK(dec);
}


gboolean gst_nonstream_audio_decoder_set_output_audioinfo(GstNonstreamAudioDecoder *dec, GstAudioInfo const *audio_info)
{
	GstCaps *caps;
	GstCaps *templ_caps;
	gboolean caps_ok;
	gboolean res = TRUE;

	g_return_val_if_fail(GST_IS_NONSTREAM_AUDIO_DECODER(dec), FALSE);

	GST_NONSTREAM_AUDIO_DECODER_STREAM_LOCK(dec);

	caps = gst_audio_info_to_caps(audio_info);
	if (caps == NULL)
	{
		GST_WARNING_OBJECT(dec, "Could not create caps out of audio info");
		res = FALSE;
		goto done;
	}

	templ_caps = gst_pad_get_pad_template_caps(dec->srcpad);
	caps_ok = gst_caps_is_subset(caps, templ_caps);

	if (caps_ok)
	{
		dec->audio_info = *audio_info;
		dec->output_format_changed = TRUE;

		GST_INFO_OBJECT(dec, "setting output format to %" GST_PTR_FORMAT, caps);
	}
	else
	{
		GST_WARNING_OBJECT(
			dec,
			"requested output format %" GST_PTR_FORMAT " do not match template %" GST_PTR_FORMAT,
			caps, templ_caps
		);

		res = FALSE;
	}

	gst_caps_unref(caps);
	gst_caps_unref(templ_caps);

done:
	GST_NONSTREAM_AUDIO_DECODER_STREAM_UNLOCK(dec);
	return res;
}


gboolean gst_nonstream_audio_decoder_negotiate(GstNonstreamAudioDecoder *dec)
{
	GstNonstreamAudioDecoderClass *klass;
	gboolean res = TRUE;

	g_return_val_if_fail(GST_IS_NONSTREAM_AUDIO_DECODER(dec), FALSE);

	klass = GST_NONSTREAM_AUDIO_DECODER_GET_CLASS(dec);

	GST_NONSTREAM_AUDIO_DECODER_STREAM_LOCK(dec);
	if (klass->negotiate != NULL)
		res = klass->negotiate(dec);
	GST_NONSTREAM_AUDIO_DECODER_STREAM_UNLOCK(dec);

	return res;
}


void gst_nonstream_audio_decoder_get_downstream_format(GstNonstreamAudioDecoder *dec, gint *sample_rate, gint *num_channels)
{
	GstCaps *allowed_srccaps;
	guint structure_nr, num_structures;
	gboolean ds_rate_found, ds_channels_found;

	g_return_if_fail(GST_IS_NONSTREAM_AUDIO_DECODER(dec));

	/* Get the caps that are allowed by downstream */
	{
		GstCaps *allowed_srccaps_unnorm = gst_pad_get_allowed_caps(dec->srcpad);
		allowed_srccaps = gst_caps_normalize(allowed_srccaps_unnorm);
	}

	ds_rate_found = FALSE;
	ds_channels_found = FALSE;

	/* Go through all allowed caps, see if one of them has sample rate or number of channels set (or both) */
	num_structures = gst_caps_get_size(allowed_srccaps);
	GST_DEBUG_OBJECT(dec, "%u structure(s) in downstream caps", num_structures);
	for (structure_nr = 0; structure_nr < num_structures; ++structure_nr)
	{
		GstStructure *structure;

		ds_rate_found = FALSE;
		ds_channels_found = FALSE;

		structure = gst_caps_get_structure(allowed_srccaps, structure_nr);

		if ((sample_rate != NULL) && gst_structure_get_int(structure, "rate", sample_rate))
		{
			GST_DEBUG_OBJECT(dec, "got sample rate from structure #%u : %d Hz", structure_nr, *sample_rate);
			ds_rate_found = TRUE;
		}
		if ((num_channels != NULL) && gst_structure_get_int(structure, "channels", num_channels))
		{
			GST_DEBUG_OBJECT(dec, "got number of channels from structure #%u : %u channels", structure_nr, *num_channels);
			ds_channels_found = TRUE;
		}

		if (ds_rate_found || ds_channels_found)
			break;
	}

	gst_caps_unref(allowed_srccaps);

	if ((sample_rate != NULL) && !ds_rate_found)
		GST_DEBUG_OBJECT(dec, "downstream did not specify sample rate - using default (%d Hz)", *sample_rate);
	if ((num_channels != NULL) && !ds_channels_found)
		GST_DEBUG_OBJECT(dec, "downstream did not specify number of channels - using default (%d channels)", *num_channels);

	
}


GstBuffer * gst_nonstream_audio_decoder_allocate_output_buffer(GstNonstreamAudioDecoder *dec, gsize size)
{
	GstBuffer *buffer = NULL;

	GST_NONSTREAM_AUDIO_DECODER_STREAM_LOCK(dec);

	if (G_UNLIKELY(
		dec->output_format_changed ||
		(GST_AUDIO_INFO_IS_VALID(&(dec->audio_info)) && gst_pad_check_reconfigure(dec->srcpad))
	))
	{
		if (!gst_nonstream_audio_decoder_negotiate(dec))
			goto done;
	}

	buffer = gst_buffer_new_allocate(dec->allocator, size, &(dec->allocation_params));

done:
	GST_NONSTREAM_AUDIO_DECODER_STREAM_UNLOCK(dec);
	return buffer;
}

