#include <string.h>
#include <config.h>
#include <gst/gst.h>
#include <gst/base/gstadapter.h>

#include "umxparse.h"


GST_DEBUG_CATEGORY_STATIC(umxparse_debug);
#define GST_CAT_DEFAULT umxparse_debug


#define umx_media_type "application/x-unreal"


static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
	"sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS(umx_media_type)
);

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
	"src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS(
		"audio/x-mod, "
		"type = (string) { mod, s3m, xm, it }; "
	)
);



typedef gint64 umx_index;


typedef struct
{
	umx_index object_name;
}
umx_import;



G_DEFINE_TYPE(GstUmxParse, gst_umx_parse, GST_TYPE_ELEMENT)



static void gst_umx_parse_finalize(GObject *object);

static gboolean gst_umx_parse_sink_event(GstPad *pad, GstObject *parent, GstEvent *event);
static GstFlowReturn gst_umx_parse_chain(GstPad *pad, GstObject *parent, GstBuffer *buffer);
static gboolean gst_umx_parse_src_query(GstPad *pad, GstObject *parent, GstQuery *query);

static gboolean gst_umx_parse_get_upstream_size(GstUmxParse *umx_parse, gint64 *length);

static GstBuffer* gst_umx_parse_read(GstUmxParse *umx_parse, GstBuffer *umx_data, GstCaps **caps);
static umx_index gst_umx_parse_read_index(guint8 *data, gsize *bufofs);



void gst_umx_parse_class_init(GstUmxParseClass *klass)
{
	GObjectClass *object_class;
	GstElementClass *element_class;

	GST_DEBUG_CATEGORY_INIT(umxparse_debug, "umxparse", 0, "Unreal UMX parser");

	object_class = G_OBJECT_CLASS(klass);
	element_class = GST_ELEMENT_CLASS(klass);

	gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&sink_template));
	gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&src_template));

	object_class->finalize = GST_DEBUG_FUNCPTR(gst_umx_parse_finalize);

	gst_element_class_set_static_metadata(
		element_class,
		"Unreal UMX parser",
		"Codec/Demuxer",
		"Parses Unreal UMX legacy music files and extracts the module music contained within",
		"Carlos Rafael Giani <dv@pseudoterminal.org>"
	);
}


void gst_umx_parse_init(GstUmxParse *umx_parse)
{
	umx_parse->upstream_eos = FALSE;
	umx_parse->module_data_size = 0;

	umx_parse->adapter = gst_adapter_new();
	umx_parse->upstream_size = -1;

	umx_parse->sinkpad = gst_pad_new_from_static_template(&sink_template, "sink");
	gst_pad_set_event_function(umx_parse->sinkpad, GST_DEBUG_FUNCPTR(gst_umx_parse_sink_event));
	gst_pad_set_chain_function(umx_parse->sinkpad, GST_DEBUG_FUNCPTR(gst_umx_parse_chain));
	gst_element_add_pad(GST_ELEMENT(umx_parse), umx_parse->sinkpad);

	umx_parse->srcpad = gst_pad_new_from_static_template(&src_template, "src");
	gst_pad_set_query_function(umx_parse->srcpad, GST_DEBUG_FUNCPTR(gst_umx_parse_src_query));
	gst_pad_use_fixed_caps(umx_parse->srcpad);	
	gst_element_add_pad(GST_ELEMENT(umx_parse), umx_parse->srcpad);
}


static void gst_umx_parse_finalize(GObject *object)
{
	GstUmxParse *umx_parse = GST_UMX_PARSE(object);

	g_object_unref(G_OBJECT(umx_parse->adapter));

	G_OBJECT_CLASS(gst_umx_parse_parent_class)->finalize(object);
}


static gboolean gst_umx_parse_sink_event(GstPad *pad, GstObject *parent, GstEvent *event)
{
	GstUmxParse *umx_parse = GST_UMX_PARSE(parent);

	switch(GST_EVENT_TYPE(event))
	{
		case GST_EVENT_SEGMENT:
			/* Upstream sends in a byte segment, which is uninteresting here,
			 * since a custom segment event is generated anyway */
			gst_event_unref(event);
			return TRUE;

		case GST_EVENT_EOS:
		{
			umx_parse->upstream_eos = TRUE;
			gst_event_unref(event);
			return TRUE;
		}

		default:
			return gst_pad_event_default(pad, parent, event);
	}
}


static GstFlowReturn gst_umx_parse_chain(G_GNUC_UNUSED GstPad *pad, GstObject *parent, GstBuffer *buffer)
{
	GstUmxParse *umx_parse = GST_UMX_PARSE(parent);

	GST_TRACE_OBJECT(umx_parse, "entered chain function");

	if (umx_parse->upstream_size < 0)
	{
		if (!gst_umx_parse_get_upstream_size(umx_parse, &(umx_parse->upstream_size)))
		{
			GST_ELEMENT_ERROR(umx_parse, STREAM, DECODE, (NULL), ("Cannot load - upstream size (in bytes) could not be determined"));
			return GST_FLOW_ERROR;
		}
	}

	/* Accumulate data until end-of-stream or the upstream size is reached, then load media and commence playback. */

	gint64 avail_size;

	gst_adapter_push(umx_parse->adapter, buffer);
	buffer = NULL;
	avail_size = gst_adapter_available(umx_parse->adapter);
	if (umx_parse->upstream_eos || (avail_size >= umx_parse->upstream_size))
	{
		GstFlowReturn flow_ret;
		GstCaps *caps;
		GstSegment segment;
		GstBuffer *adapter_buffer, *module_data;

		/* Take all data from the adapter */
		adapter_buffer = gst_adapter_take_buffer(umx_parse->adapter, avail_size);
		module_data = gst_umx_parse_read(umx_parse, adapter_buffer, &caps);
		gst_buffer_unref(adapter_buffer);

		if (module_data == NULL)
			return GST_FLOW_ERROR;

		/* Record module data size for duration queries */
		umx_parse->module_data_size = gst_buffer_get_size(module_data);

		/* Send new caps downstream */
		gst_pad_push_event(umx_parse->srcpad, gst_event_new_caps(caps));
		gst_caps_unref(caps);

		/* Start new segment */
		gst_segment_init(&segment, GST_FORMAT_BYTES);
		segment.duration = umx_parse->module_data_size;
		gst_pad_push_event(umx_parse->srcpad, gst_event_new_segment(&segment));

		/* Push the actual module data downstream */
		flow_ret = gst_pad_push(umx_parse->srcpad, module_data);
		if (flow_ret != GST_FLOW_OK)
		{
			GST_ERROR_OBJECT(umx_parse, "failed to push module data downstream: %s (%d)", gst_flow_get_name(flow_ret), flow_ret);
			return flow_ret;
		}

		/* Finish delivery with an EOS event (since all data
		 * that could be transmitted has been transmitted) */
		gst_pad_push_event(umx_parse->srcpad, gst_event_new_eos());
	}

	return GST_FLOW_OK;
}


static gboolean gst_umx_parse_src_query(GstPad *pad, GstObject *parent, GstQuery *query)
{
	gboolean res;
	GstFormat format;
	GstUmxParse *umx_parse = GST_UMX_PARSE(parent);

	res = FALSE;

	switch (GST_QUERY_TYPE(query))
	{
		case GST_QUERY_DURATION:
		{
			gst_query_parse_duration(query, &format, NULL);
			GST_DEBUG_OBJECT(umx_parse, "got duration query, format: %s", gst_format_get_name(format));
			if ((format == GST_FORMAT_BYTES) && (umx_parse->module_data_size >= 0))
			{
				GST_DEBUG_OBJECT(umx_parse, "responding to query with size %" G_GINT64_FORMAT, umx_parse->module_data_size);
				gst_query_set_duration(query, format, umx_parse->module_data_size);
				res = TRUE;
			}
			else
				GST_DEBUG_OBJECT(umx_parse, "cannot respond to query, no size set or query format is not in bytes");

			break;
		}
		default:
			break;
	}

	if (!res)
		res = gst_pad_query_default(pad, parent, query);

	return res;
}


static gboolean gst_umx_parse_get_upstream_size(GstUmxParse *umx_parse, gint64 *length)
{
	return gst_pad_peer_query_duration(umx_parse->sinkpad, GST_FORMAT_BYTES, length) && (*length >= 0);
}


static GstBuffer* gst_umx_parse_read(GstUmxParse *umx_parse, GstBuffer *umx_data, GstCaps **caps)
{
	gchar *mod_type;
	umx_index offset, size;
	GstMapInfo in_map;


	/* The UMX parser was written using these specifications and examples:
	 * http://wiki.beyondunreal.com/Legacy:Package_File_Format
	 * http://wiki.beyondunreal.com/Unreal_package
	 * http://sourceforge.net/p/modplug/code/HEAD/tree/trunk/OpenMPT/soundlib/Load_umx.cpp
	 */


	gst_buffer_map(umx_data, &in_map, GST_MAP_READ);

	{
		gchar **names;
		umx_import *imports;

		guint32 const expected_magic_id = 0x9E2A83C1;
		guint32 magic_id, num_names, names_offset, num_exports, exports_offset, num_imports, imports_offset;
		guint16 pkg_version;
		guint32 i;

		/* bufofs is the current "read" position in the buffer
		 * "seeking" is done by manipulating its value */
		gsize bufofs = 0;

		/* read the 32-bit integer  magic ID to identify an Unreal package */
		magic_id = GST_READ_UINT32_LE(in_map.data); bufofs += 4;
		if (magic_id != expected_magic_id)
		{
			gst_buffer_unmap(umx_data, &in_map);
			GST_ERROR_OBJECT(umx_parse, "expected signature 0x%x, found 0x%x", expected_magic_id, magic_id);
			return NULL;
		}

		/* read the 16-bit integer package version (needed below) */
		pkg_version = GST_READ_UINT16_LE(in_map.data + bufofs); bufofs += 2;
		GST_DEBUG_OBJECT(umx_parse, "package version: %u", pkg_version);

		bufofs += 2; /* 16-bit integer containing license mode; uninteresting, skip */
		bufofs += 4; /* 32-bit integer containing package flags; uninteresting, skip */

		/* offset of tables and number of entries in tables */
		num_names      = GST_READ_UINT32_LE(in_map.data + bufofs); bufofs += 4;
		names_offset   = GST_READ_UINT32_LE(in_map.data + bufofs); bufofs += 4;
		num_exports    = GST_READ_UINT32_LE(in_map.data + bufofs); bufofs += 4;
		exports_offset = GST_READ_UINT32_LE(in_map.data + bufofs); bufofs += 4;
		num_imports    = GST_READ_UINT32_LE(in_map.data + bufofs); bufofs += 4;
		imports_offset = GST_READ_UINT32_LE(in_map.data + bufofs); bufofs += 4;

		GST_DEBUG_OBJECT(
			umx_parse, "%u names at 0x%x  %u exports at 0x%x  %u imports at 0x%x",
			num_names, names_offset,
			num_exports, exports_offset,
			num_imports, imports_offset
		);

		names = g_slice_alloc(sizeof(gchar*) * num_names);
		imports = g_slice_alloc(sizeof(umx_import) * num_imports);

		/* read all names from the name table
		 * the name table associates an index (the array index) with a string
		 * so, name[i] = "foo"; associates "foo" with index i */
		bufofs = names_offset;
		for (i = 0; i < num_names; ++i)
		{
			if (pkg_version < 64)
			{
				names[i] = (gchar *)(in_map.data + bufofs);
				bufofs += strlen(names[i]) + 1 + 4; /* string + nullbyte + 32bit flags value */
			}
			else
			{
				int slen = in_map.data[bufofs];
				names[i] = (gchar *)(in_map.data + bufofs + 1);
				bufofs += 1 + slen + 4; /* string length in first byte (includes nullbyte) + string + 32bit flags value */
			}

			GST_DEBUG_OBJECT(umx_parse, "name #%u: \"%s\"", i, names[i]);
		}

		/* read all imports from the import table
		 * it is needed for identifying the module type
		 * and to verify that the data is of type "Music" */
		bufofs = imports_offset;
		for (i = 0; i < num_imports; ++i)
		{
			umx_import *im = &(imports[i]);
			gst_umx_parse_read_index(in_map.data, &bufofs); /* skip class package */
			gst_umx_parse_read_index(in_map.data, &bufofs); /* skip class name */
			bufofs += 4; /* skip package */
			im->object_name = gst_umx_parse_read_index(in_map.data, &bufofs);
		}

		/* these two variables will contain the offset where the module
		 * music block is and how large it is */
		offset = 0;
		size = 0;

		/* read all exports from the export table
		 * the export table is where the actual data is contained */
		bufofs = exports_offset;
		for (i = 0; i < num_exports; ++i)
		{
			umx_index umx_class, serial_size, serial_offset, chunk_size;
			gint64 im_idx;
			umx_import *im;
			gchar *name;

			umx_class = gst_umx_parse_read_index(in_map.data, &bufofs);
			gst_umx_parse_read_index(in_map.data, &bufofs); /* skip super index */
			bufofs += 4; /* skip group */
			gst_umx_parse_read_index(in_map.data, &bufofs); /* skip object name */
			bufofs += 4; /* skip object flags */
			serial_size = gst_umx_parse_read_index(in_map.data, &bufofs);
			if (serial_size <= 0)
				continue;

			serial_offset = gst_umx_parse_read_index(in_map.data, &bufofs);

			/* verify that the data is a valid music block */
			im_idx = -umx_class - 1;
			im = &(imports[im_idx]);
			name = names[im->object_name];

			/* data blocks other than Music are not relevant */
			if (g_strcmp0(name, "Music") != 0)
				continue;

			/* retrieve the module type */
			mod_type = names[0];

			/* skip to the serialized data */
			bufofs = serial_offset;

			gst_umx_parse_read_index(in_map.data, &bufofs); /* skip number of properties */

			/* skip unused data, depending on the package version
			 * taken from OpenMPT's Load_umx.cpp */
			if (pkg_version >= 120)
			{
				/* UT2003 packages */
				gst_umx_parse_read_index(in_map.data, &bufofs);
				bufofs += 8;
			}
			else if (pkg_version >= 100)
			{
				/* AAO packages */
				bufofs += 4;
				gst_umx_parse_read_index(in_map.data, &bufofs);
				bufofs += 4;
			}
			else if (pkg_version >= 62)
			{
				/* UT packages
				 * Mech8.umx and a few other UT tunes have packageVersion = 62.
				 * In CUnSound.cpp, the condition above reads "packageVersion >= 63"
				 * but if that is used, those tunes won't load properly.
				 */
				gst_umx_parse_read_index(in_map.data, &bufofs);
				bufofs += 4;
			}
			else
			{
				/* old Unreal packages */
				gst_umx_parse_read_index(in_map.data, &bufofs);
			}

			chunk_size = gst_umx_parse_read_index(in_map.data, &bufofs);

			/* finally, these are the offset and the size of the
			 * actual module data within the Unreal package */
			offset = bufofs;
			size = chunk_size;

			GST_DEBUG_OBJECT(
				umx_parse,
				"found music data at offset %" G_GINT64_FORMAT " size %" G_GINT64_FORMAT " (serial size: %" G_GINT64_FORMAT " (%" G_GINT64_FORMAT " without chunk metadata)  chunk size: %" G_GINT64_FORMAT ")",
				offset,
				size,
				serial_size,
				serial_size - (bufofs - serial_offset),
				chunk_size
			);

			break;
		}

		/* cleanup */
		g_slice_free1(sizeof(gchar*) * num_names, names);
		g_slice_free1(sizeof(umx_import) * num_imports, imports);

		if (size == 0)
		{
			GST_ERROR_OBJECT(umx_parse, "no valid music data found");
			gst_buffer_unmap(umx_data, &in_map);
			return NULL;
		}
	}

	if (caps != NULL)
		*caps = gst_caps_new_simple("audio/x-mod", "type", G_TYPE_STRING, mod_type, NULL);

	/* unmapping AFTER creating caps, since otherwise mod_type would be invalid */
	gst_buffer_unmap(umx_data, &in_map);

	/* copy region within the umx data which contains the actual module */
	return gst_buffer_copy_region(umx_data, GST_BUFFER_COPY_MEMORY, offset, size);
}


static umx_index gst_umx_parse_read_index(guint8 *data, gsize *bufofs)
{
	int i;
	umx_index idx = 0;
	gboolean sign = FALSE, more_bytes = TRUE;

	for (i = 0; i < 5; ++i)
	{
		guint8 byte;
		
		byte = data[*bufofs];
		(*bufofs)++;

		switch (i)
		{
			case 0:
			{
				sign = (byte & 0x80);
				idx |= byte & 0x3f;
				more_bytes = (byte & 0x40);
				break;
			}
			default:
			{
				idx |= (gint64)(byte & 0x7f) << (6 + ((i - 1) * 7));
				more_bytes = (byte & 0x80);
			}
		}

		if (!more_bytes)
			break;
	}

	idx *= sign ? - 1 : 1;

	return idx;
}


static void gst_umx_parse_type_find(GstTypeFind *tf, G_GNUC_UNUSED gpointer user_data)
{
	const guint8 *data;

	if ((data = gst_type_find_peek(tf, 0, 4)) != NULL)
	{
		if (memcmp(data, "\xC1\x83\x2A\x9E", 4) == 0)
			gst_type_find_suggest_simple(tf, GST_TYPE_FIND_LIKELY, umx_media_type, NULL);
	}
}





static gboolean plugin_init(GstPlugin *plugin)
{
	if (!gst_element_register(plugin, "umxparse", GST_RANK_PRIMARY + 1, gst_umx_parse_get_type()))
		return FALSE;
	if (!gst_type_find_register(plugin, umx_media_type, GST_RANK_PRIMARY, gst_umx_parse_type_find, "umx", NULL, NULL, NULL))
		return FALSE;

	return TRUE;
}

GST_PLUGIN_DEFINE(
	GST_VERSION_MAJOR,
	GST_VERSION_MINOR,
	umxparse,
	"Unreal UMX parser",
	plugin_init,
	"1.0",
	"LGPL",
	"package",
	"http://no-url-yet"
)

