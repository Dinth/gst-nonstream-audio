#include <stdlib.h>
#include <string.h>
#include <config.h>

#include <gst/gst.h>

#include "gstdumbdec.h"


/* TODO: looping is still glitchy; perhaps use the "open-ended" looping mode? */


GST_DEBUG_CATEGORY_STATIC(dumbdec_debug);
#define GST_CAT_DEFAULT dumbdec_debug


#define DEFAULT_SAMPLE_RATE 48000
#define DEFAULT_NUM_CHANNELS 2

#define RENDER_BIT_DEPTH 16
#define AUDIO_FORMAT GST_AUDIO_FORMAT_S16



static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
	"sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS(
		"audio/x-mod"
	)
);

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
	"src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS(
		"audio/x-raw, "
		"format = (string) " GST_AUDIO_NE(S16) ", "
		"layout = (string) interleaved, "
		"rate = (int) [ 1, 48000 ], "
		"channels = (int) [ 1, 2 ] "
	)
);



G_DEFINE_TYPE(GstDumbDec, gst_dumb_dec, GST_TYPE_NONSTREAM_AUDIO_DECODER)



static gboolean gst_dumb_dec_seek(GstNonstreamAudioDecoder *dec, GstClockTime new_position);
static GstClockTime gst_dumb_dec_tell(GstNonstreamAudioDecoder *dec);

static gboolean gst_dumb_dec_load(GstNonstreamAudioDecoder *dec, GstBuffer *source_data);

static gboolean gst_dumb_dec_set_num_loops(GstNonstreamAudioDecoder *dec, gint num_loops);

static gboolean gst_dumb_dec_decode(GstNonstreamAudioDecoder *dec, GstBuffer **buffer, guint *num_samples);

static gboolean gst_dumb_dec_init_sigrenderer(GstDumbDec *dumb_dec, GstClockTime seek_pos);



void gst_dumb_dec_class_init(GstDumbDecClass *klass)
{
	GObjectClass *object_class;
	GstElementClass *element_class;
	GstNonstreamAudioDecoderClass *dec_class;

	GST_DEBUG_CATEGORY_INIT(dumbdec_debug, "dumbdec", 0, "DUMB module player");

	object_class = G_OBJECT_CLASS(klass);
	element_class = GST_ELEMENT_CLASS(klass);
	dec_class = GST_NONSTREAM_AUDIO_DECODER_CLASS(klass);

	gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&sink_template));
	gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&src_template));

	object_class->set_property = GST_DEBUG_FUNCPTR(gst_nonstream_audio_decoder_set_loop_property);
	object_class->get_property = GST_DEBUG_FUNCPTR(gst_nonstream_audio_decoder_get_loop_property);

	dec_class->seek = GST_DEBUG_FUNCPTR(gst_dumb_dec_seek);
	dec_class->tell = GST_DEBUG_FUNCPTR(gst_dumb_dec_tell);
	dec_class->load = GST_DEBUG_FUNCPTR(gst_dumb_dec_load);
	dec_class->set_num_loops = GST_DEBUG_FUNCPTR(gst_dumb_dec_set_num_loops);
	dec_class->decode = GST_DEBUG_FUNCPTR(gst_dumb_dec_decode);

	gst_nonstream_audio_decoder_init_loop_properties(dec_class, FALSE);

	gst_element_class_set_static_metadata(
		element_class,
		"DUMB module player",
		"Codec/Decoder/Audio",
		"Playes module files (MOD/S3M/XM/IT/MTM/...) using the DUMB (Dynamic Universal Music Bibliotheque) library",
		"Carlos Rafael Giani <dv@pseudoterminal.org>"
	);
}


void gst_dumb_dec_init(GstDumbDec *dumb_dec)
{
	dumb_dec->cur_loop_count = 0;
	dumb_dec->num_loops = 0;
	dumb_dec->loop_end_reached = FALSE;
	dumb_dec->duh = NULL;
	dumb_dec->duh_sigrenderer = NULL;

	/* TODO: remove these lines */
	GST_NONSTREAM_AUDIO_DECODER(dumb_dec)->num_loops = 2;
	dumb_dec->num_loops = 2;
}


static gboolean gst_dumb_dec_seek(GstNonstreamAudioDecoder *dec, GstClockTime new_position)
{
	if (!gst_dumb_dec_init_sigrenderer(GST_DUMB_DEC(dec), new_position))
	{
		GST_ELEMENT_ERROR(dec, STREAM, DECODE, (NULL), ("cannot reinitialize DUMB decoding"));
		return FALSE;
	}

	return TRUE;
}


static GstClockTime gst_dumb_dec_tell(GstNonstreamAudioDecoder *dec)
{
	GstClockTime pos;
	GstDumbDec *dumb_dec = GST_DUMB_DEC(dec);

	if (dumb_dec->duh_sigrenderer == NULL)
		return 0;

	pos = duh_sigrenderer_get_position(dumb_dec->duh_sigrenderer) % duh_get_length(dumb_dec->duh);
	GST_DEBUG_OBJECT(dec, "pos: %u len: %u", pos, duh_get_length(dumb_dec->duh));
	pos = pos * GST_SECOND / 65536;

	return pos;
}


static gboolean gst_dumb_dec_load(GstNonstreamAudioDecoder *dec, GstBuffer *source_data)
{
	GstDumbDec *dumb_dec = GST_DUMB_DEC(dec);

	dumb_dec->sample_rate = DEFAULT_SAMPLE_RATE;
	dumb_dec->num_channels = DEFAULT_NUM_CHANNELS;
	gst_nonstream_audio_decoder_get_downstream_format(dec, &(dumb_dec->sample_rate), &(dumb_dec->num_channels));

	{
		GstMapInfo map;
		DUMBFILE *dumbfile;

		gst_buffer_map(source_data, &map, GST_MAP_READ);
		dumbfile = dumbfile_open_memory((char const *)(map.data), map.size);

		dumb_dec->duh = dumb_read_any(dumbfile, 0/*restrict_*/, 0/*subsong*/);

		dumbfile_close(dumbfile);
		gst_buffer_unmap(source_data, &map);

		if (dumb_dec->duh == NULL)
		{
			GST_ELEMENT_ERROR(dumb_dec, STREAM, DECODE, (NULL), ("DUMB failed to read module data"));
			return FALSE;
		}
	}

	if (!gst_dumb_dec_init_sigrenderer(dumb_dec, 0))
	{
		GST_ELEMENT_ERROR(dumb_dec, STREAM, DECODE, (NULL), ("cannot initialize DUMB decoding"));
		return FALSE;
	}

	/* Set output format */
	{
		GstAudioInfo audio_info;

		gst_audio_info_init(&audio_info);

		gst_audio_info_set_format(
			&audio_info,
			GST_AUDIO_FORMAT_S16,
			dumb_dec->sample_rate,
			dumb_dec->num_channels,
			NULL
		);

		if (!gst_nonstream_audio_decoder_set_output_audioinfo(dec, &audio_info))
			return FALSE;
	}

	gst_nonstream_audio_decoder_set_duration(dec, duh_get_length(dumb_dec->duh) * GST_SECOND / 65536);

	{
		char const *title, *message;
		GstTagList *tags;

		tags = gst_tag_list_new_empty();

		title = duh_get_tag(dumb_dec->duh, "TITLE");
		if (title != NULL)
			gst_tag_list_add(tags, GST_TAG_MERGE_APPEND, GST_TAG_TITLE, title, NULL);

		message = (char const*)(dumb_it_sd_get_song_message(duh_get_it_sigdata(dumb_dec->duh)));
		if (message != NULL)
			gst_tag_list_add(tags, GST_TAG_MERGE_APPEND, GST_TAG_COMMENT, message, NULL);

		gst_pad_push_event(GST_NONSTREAM_AUDIO_DECODER_SRC_PAD(dumb_dec), gst_event_new_tag(tags));
	}


	return TRUE;
}


static gboolean gst_dumb_dec_set_num_loops(GstNonstreamAudioDecoder *dec, gint num_loops)
{
	GstDumbDec *dumb_dec;

	dumb_dec = GST_DUMB_DEC(dec);

	if ((num_loops < 1) || (dumb_dec->cur_loop_count >= num_loops))
		dumb_dec->cur_loop_count = 0;
	dumb_dec->num_loops = num_loops;

	return TRUE;
}


static gboolean gst_dumb_dec_decode(GstNonstreamAudioDecoder *dec, GstBuffer **buffer, guint *num_samples)
{
	GstDumbDec *dumb_dec;
	GstBuffer *outbuf;
	GstMapInfo map;
	gint num_samples_per_outbuf, num_bytes_per_outbuf, actual_num_samples_read;

	dumb_dec = GST_DUMB_DEC(dec);

	if (dumb_dec->loop_end_reached)
	{
		dumb_dec->loop_end_reached = FALSE;
		gst_nonstream_audio_decoder_handle_loop(dec, gst_dumb_dec_tell(dec));
	}

	num_samples_per_outbuf = 1024;
	{
		long len = duh_get_length(dumb_dec->duh);
		long pos = duh_sigrenderer_get_position(dumb_dec->duh_sigrenderer) % len;
		long diff = len - pos;
		if (diff < num_samples_per_outbuf)
		{
			GST_DEBUG_OBJECT(dumb_dec, "Truncated end length found: %d", diff);
			num_samples_per_outbuf = diff;
		}
	}
	num_bytes_per_outbuf = num_samples_per_outbuf * dumb_dec->num_channels * RENDER_BIT_DEPTH / 8;

	outbuf = gst_nonstream_audio_decoder_allocate_output_buffer(dec, num_bytes_per_outbuf);
	if (outbuf == NULL)
		return FALSE;

	gst_buffer_map(outbuf, &map, GST_MAP_WRITE);
	actual_num_samples_read = duh_render(dumb_dec->duh_sigrenderer, RENDER_BIT_DEPTH, 0, 1.0f, 65536.0f / dumb_dec->sample_rate, num_samples_per_outbuf, map.data);
	gst_buffer_unmap(outbuf, &map);

	if (actual_num_samples_read == 0)
	{
		gst_buffer_unref(outbuf);
		GST_INFO_OBJECT(dumb_dec, "DUMB reached end of module");
		return FALSE;
	}
	else
	{
		*buffer = outbuf;
		*num_samples = actual_num_samples_read;
		return TRUE;
	}
}


static int gst_dumb_dec_loop_callback(void *ptr)
{
	gboolean continue_loop;
	GstDumbDec *dumb_dec;
	//GstNonstreamAudioDecoder *dec;

	dumb_dec = (GstDumbDec*)(ptr);
	//dec = GST_NONSTREAM_AUDIO_DECODER(dumb_dec);

	GST_DEBUG_OBJECT(dumb_dec, "DUMB reached loop callback");

	if (dumb_dec->num_loops < 0)
		continue_loop = TRUE;
	else if (dumb_dec->num_loops == 0)
		continue_loop = FALSE;
	else
	{
		if (dumb_dec->cur_loop_count >= dumb_dec->num_loops)
			continue_loop = FALSE;
		else
		{
			++dumb_dec->cur_loop_count;
			continue_loop = TRUE;
		}
	}

	if (continue_loop)
	{
		dumb_dec->loop_end_reached = TRUE;
		//gst_nonstream_audio_decoder_handle_loop(dec, 0);
	}

	return continue_loop ? 0 : 1;
}


static gboolean gst_dumb_dec_init_sigrenderer(GstDumbDec *dumb_dec, GstClockTime seek_pos)
{
	g_return_val_if_fail(dumb_dec->duh != NULL, FALSE);

	if (dumb_dec->duh_sigrenderer != NULL)
		duh_end_sigrenderer(dumb_dec->duh_sigrenderer);
	dumb_dec->duh_sigrenderer = duh_start_sigrenderer(dumb_dec->duh, 0, dumb_dec->num_channels, seek_pos * 65536 / GST_SECOND);

	if (dumb_dec->duh_sigrenderer == NULL)
		return FALSE;

	dumb_dec->cur_loop_count = 0;
	dumb_dec->loop_end_reached = FALSE;

	{
		DUMB_IT_SIGRENDERER *itsr = duh_get_it_sigrenderer(dumb_dec->duh_sigrenderer);
		dumb_it_set_loop_callback(itsr, &gst_dumb_dec_loop_callback, dumb_dec);
		dumb_it_set_xm_speed_zero_callback(itsr, &gst_dumb_dec_loop_callback, dumb_dec);
		dumb_it_set_global_volume_zero_callback(itsr, &gst_dumb_dec_loop_callback, dumb_dec);
	}

	return TRUE;
}





static gboolean plugin_init(GstPlugin *plugin)
{
	if (!gst_element_register(plugin, "dumbdec", GST_RANK_PRIMARY + 1, gst_dumb_dec_get_type())) return FALSE;
	return TRUE;
}

GST_PLUGIN_DEFINE(
	GST_VERSION_MAJOR,
	GST_VERSION_MINOR,
	dumbdec,
	"DUMB (Dynamic Universal Music Bibliotheque) module player",
	plugin_init,
	"1.0",
	"LGPL",
	"package",
	"http://no-url-yet"
)

