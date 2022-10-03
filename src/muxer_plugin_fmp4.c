#include "muxer_plugin_fmp4.h"

#include "minifmp4.h"

#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include "map.h"
#include "id3.h"

static const char* AOID3_SCHEME_ID_URI = "https://aomedia.org/emsg/ID3";
static const char* AOID3_VALUE = "0";

static map measurement_method_keys;
static map measurement_system_keys;
static map reliability_keys;

#define CONFIGURING_MAIN 0
#define CONFIGURING_LOUDNESS 1
#define CONFIGURING_MEASUREMENT 2

static STRBUF_CONST(mime_mp4,"audio/mp4");
static STRBUF_CONST(mime_m4s,"audio/mp4");
static STRBUF_CONST(ext_mp4,".mp4");
static STRBUF_CONST(ext_m4s,".m4s");

#define KEY(v,t) static STRBUF_CONST(KEY_##v,#t)

KEY(loudness,loudness);
KEY(track,track);
KEY(album,album);
KEY(true_peak,true-peak);
KEY(sample_peak,sample-peak);
KEY(measurement_system,measurement-system);
KEY(reliability,reliability);
KEY(measurement_method,measurement-method);
KEY(value,value);

struct plugin_userdata {
    fmp4_mux mux;
    fmp4_track* track;
    fmp4_loudness* loudness;
    fmp4_measurement* measurement;
    fmp4_emsg* emsg;
    membuf expired_emsgs;
    size_t samples_per_segment;
    unsigned int segment_length;
    uint8_t configuring;
    id3 id3;
};

typedef struct plugin_userdata plugin_userdata;

static void* plugin_create(void) {
    plugin_userdata* userdata = (plugin_userdata*)malloc(sizeof(plugin_userdata));
    if(userdata == NULL) return NULL;
    fmp4_mux_init(&userdata->mux, NULL);

    id3_init(&userdata->id3);
    if(id3_ready(&userdata->id3) != 0) {
        free(userdata); return NULL;
    }

    if( (userdata->track = fmp4_mux_new_track(&userdata->mux)) == NULL) {
        id3_free(&userdata->id3);
        free(userdata); return NULL;
    }

    if(fmp4_mux_add_brand(&userdata->mux,"aid3") != FMP4_OK) {
        fmp4_mux_close(&userdata->mux);
        id3_free(&userdata->id3);
        free(userdata); return NULL;
    }

    userdata->segment_length = 0;
    userdata->loudness = NULL;
    userdata->measurement = NULL;
    userdata->configuring = CONFIGURING_MAIN;
    userdata->emsg = NULL;

    membuf_init(&userdata->expired_emsgs);
    return userdata;
}

static void expire_emsgs(plugin_userdata* userdata) {
    size_t i = 0;
    size_t len = userdata->expired_emsgs.len / sizeof(fmp4_emsg*);
    fmp4_emsg** e = (fmp4_emsg**)userdata->expired_emsgs.x;
    for(i=0;i<len;i++) {
        fmp4_emsg_free(e[i]);
    }
    membuf_reset(&userdata->expired_emsgs);
}

static void plugin_close(void* ud) {
    plugin_userdata* userdata = (plugin_userdata*)ud;
    if(userdata->emsg != NULL) fmp4_emsg_free(userdata->emsg);
    fmp4_mux_close(&userdata->mux);
    expire_emsgs(userdata);
    membuf_free(&userdata->expired_emsgs);
    id3_free(&userdata->id3);
    free(userdata);
}

static int plugin_config_measurement(plugin_userdata* userdata, const strbuf* key, const strbuf* value) {
    fmp4_result r;
    map_entry *e = NULL;

    if(strbuf_equals(key,&KEY_value)) {
        errno = 0;
        r = fmp4_measurement_set_value(userdata->measurement,strbuf_strtod(value));
        if(r != FMP4_OK || errno) {
            errno = 0;
            fprintf(stderr,"[muxer:fmp4] error parsing measurement value %.*s\n",(int)value->len,(char*)value->x);
            return -1;
        }
        return 0;
    }

    if(strbuf_equals(key,&KEY_measurement_system)) {
        e = map_find_lc(&measurement_system_keys,value);
        if(e != NULL) {
            r = fmp4_measurement_set_system(userdata->measurement,e->value.u8);
        } else {
            errno = 0;
            r = fmp4_measurement_set_system(userdata->measurement,strbuf_strtoul(value,10));
        }
        if(r != FMP4_OK || errno) {
            errno = 0;
            fprintf(stderr,"[muxer:fmp4] error parsing measurement-system value %.*s\n",(int)value->len,(char*)value->x);
            return -1;
        }
        return 0;
    }

    if(strbuf_equals(key,&KEY_reliability)) {
        /* see if we're using a string */
        e = map_find_lc(&reliability_keys,value);
        if(e != NULL) {
            r = fmp4_measurement_set_reliability(userdata->measurement,e->value.u8);
        } else {
            errno = 0;
            r = fmp4_measurement_set_reliability(userdata->measurement,strbuf_strtoul(value,10));
        }
        if(r != FMP4_OK || errno) {
            errno = 0;
            fprintf(stderr,"[muxer:fmp4] error parsing reliability value %.*s\n",(int)value->len,(char*)value->x);
            return -1;
        }
        return 0;
    }

    return -2;
}

static int plugin_config_loudness(plugin_userdata* userdata, const strbuf* key, const strbuf* value) {
    fmp4_result r;
    map_entry *e = NULL;

    if(strbuf_equals(key,&KEY_true_peak)) {
        errno = 0;
        r = fmp4_loudness_set_true_peak(userdata->loudness,strbuf_strtod(value));
        if(r != FMP4_OK || errno) {
            errno = 0;
            fprintf(stderr,"[muxer:fmp4] error parsing true-peak value %.*s\n",(int)value->len,(char*)value->x);
            return -1;
        }
        return 0;
    }

    if(strbuf_equals(key,&KEY_sample_peak)) {
        errno = 0;
        r = fmp4_loudness_set_sample_peak(userdata->loudness,strbuf_strtod(value));
        if(r != FMP4_OK || errno) {
            errno = 0;
            fprintf(stderr,"[muxer:fmp4] error parsing sample-peak value %.*s\n",(int)value->len,(char*)value->x);
            return -1;
        }
        return 0;
    }

    if(strbuf_equals(key,&KEY_measurement_system)) {
        e = map_find_lc(&measurement_system_keys,value);
        if(e != NULL) {
            r = fmp4_loudness_set_system(userdata->loudness,e->value.u8);
        } else {
            errno = 0;
            r = fmp4_loudness_set_system(userdata->loudness,strbuf_strtoul(value,10));
        }
        if(r != FMP4_OK || errno) {
            errno = 0;
            fprintf(stderr,"[muxer:fmp4] error parsing measurement-system value %.*s\n",(int)value->len,(char*)value->x);
            return -1;
        }
        return 0;
    }

    if(strbuf_equals(key,&KEY_reliability)) {
        e = map_find_lc(&reliability_keys,value);
        if(e != NULL) {
            r = fmp4_loudness_set_reliability(userdata->loudness,e->value.u8);
        } else {
            errno = 0;
            r = fmp4_loudness_set_reliability(userdata->loudness,strbuf_strtoul(value,10));
        }
        if(r != FMP4_OK || errno) {
            errno = 0;
            fprintf(stderr,"[muxer:fmp4] error parsing reliability value %.*s\n",(int)value->len,(char*)value->x);
            return -1;
        }
        return 0;
    }

    if(strbuf_equals(key,&KEY_measurement_method)) {
        if( (userdata->measurement = fmp4_loudness_new_measurement(userdata->loudness)) == NULL) {
            fprintf(stderr,"[muxer:fmp4] error adding measurement to loudness\n");
            return -1;
        }

        e = map_find_lc(&measurement_method_keys,value);
        if(e != NULL) {
            r = fmp4_measurement_set_method(userdata->measurement,e->value.u8);
        } else {
            errno = 0;
            r = fmp4_measurement_set_method(userdata->measurement,strbuf_strtoul(value,10));
        }

        if(r != FMP4_OK || errno) {
            errno = 0;
            fprintf(stderr,"[muxer:fmp4] error parsing measurement value %.*s\n",(int)value->len,(char*)value->x);
            return -1;
        }
        userdata->configuring = CONFIGURING_MEASUREMENT;
        return 0;
    }

    return -2;
}

static int plugin_config_main(plugin_userdata* userdata, const strbuf* key, const strbuf* value) {
    if(strbuf_equals(key,&KEY_loudness)) {
        if( (userdata->loudness = fmp4_track_new_loudness(userdata->track)) == NULL) {
            fprintf(stderr,"[muxer:fmp4] error allocating new loudness\n");
            return -1;
        }
        if(strbuf_equals(value,&KEY_track)) {
            userdata->loudness->type = FMP4_LOUDNESS_TRACK;
        } else if(strbuf_equals(value,&KEY_album)) {
            userdata->loudness->type = FMP4_LOUDNESS_ALBUM;
        } else {
            fprintf(stderr,"[muxer:fmp4] unknown loudness type %.*s\n",(int)value->len,(char *)value->x);
            return -1;
        }

        userdata->configuring = CONFIGURING_LOUDNESS;
        return 0;
    }

    return -2;
}

static int plugin_config(void* ud, const strbuf* key, const strbuf* value) {
    int r = -2;
    plugin_userdata* userdata = (plugin_userdata*)ud;

    switch(userdata->configuring) {
        case CONFIGURING_MAIN: r = plugin_config_main(userdata,key,value); break;
        case CONFIGURING_LOUDNESS: r = plugin_config_loudness(userdata,key,value); break;
        case CONFIGURING_MEASUREMENT: r = plugin_config_measurement(userdata,key,value); break;
        default: break;
    }

    if(r == -2) {
        fprintf(stderr,"[muxer:fmp4] unknown config key: %.*s\n",
          (int)key->len,(char *)key->x);
        r = -1;
    }

    return r;

}

struct segment_wrapper {
    const segment_handler* handler;
    int samples;
    int time_base;
};

typedef struct segment_wrapper segment_wrapper;

static size_t plugin_write_segment_callback(const void* src, size_t len, void* userdata) {
    const segment_wrapper* wrapper = (const segment_wrapper*)userdata;
    int r;
    segment s;

    s.type = SEGMENT_TYPE_MEDIA;
    s.data = src;
    s.len = len;
    s.samples = wrapper->samples;

    r = wrapper->handler->cb(wrapper->handler->userdata,&s);
    return r == 0 ? len : 0;
}

static int plugin_submit_tags(void* ud, const taglist* tags) {
    plugin_userdata* userdata = (plugin_userdata*)ud;

    uint32_t id = 0;

    id3_reset(&userdata->id3);
    if(id3_add_taglist(&userdata->id3,tags) < 0) {
        fprintf(stderr,"had some kind of error on making a taglist!\n");
        return -1;
    }

    /* if we have an existing emsg,
     * set the event duration to the current sample count
     * and throw it in the expired list. */
    if(userdata->emsg != NULL) {
        userdata->emsg->event_duration = userdata->track->trun_sample_count;
        /* add this to the next segment write */
        if(fmp4_mux_add_emsg(&userdata->mux,userdata->emsg) != FMP4_OK) {
            return -1;
        }
        if(membuf_append(&userdata->expired_emsgs,&userdata->emsg,sizeof(fmp4_emsg*)) != 0) {
            return -1;
        }
        id = userdata->emsg->id + 1;
    }

    userdata->emsg = fmp4_emsg_new(NULL);
    if(userdata->emsg == NULL) return -1;

    userdata->emsg->version = 1;
    userdata->emsg->id = id;
    userdata->emsg->timescale = userdata->track->time_scale;
    if(fmp4_emsg_set_scheme_id_uri(userdata->emsg, AOID3_SCHEME_ID_URI) != FMP4_OK) return -1;
    if(fmp4_emsg_set_value(userdata->emsg, AOID3_VALUE) != FMP4_OK) return -1;
    userdata->emsg->presentation_time = userdata->track->base_media_decode_time + userdata->track->trun_sample_count;
    userdata->emsg->event_duration = 0;
    if(fmp4_emsg_set_message(userdata->emsg,userdata->id3.x,userdata->id3.len) != FMP4_OK) return -1;

    return 0;
}

/* used in both the flush and submit packet functions. The flush function
 * just also calls the segment_handler's flush */
static int plugin_muxer_flush(plugin_userdata* userdata, const segment_handler* handler) {
    fmp4_result res;
    segment_wrapper wrapper;

    wrapper.handler = handler;
    wrapper.samples = userdata->track->trun_sample_count;
    wrapper.time_base = userdata->track->time_scale;

    if(userdata->emsg != NULL) {
        userdata->emsg->event_duration = userdata->track->trun_sample_count - (userdata->emsg->presentation_time - userdata->track->base_media_decode_time);
        if(fmp4_mux_add_emsg(&userdata->mux,userdata->emsg) != FMP4_OK) return -1;
    }

    res = fmp4_mux_write_segment(&userdata->mux, plugin_write_segment_callback, &wrapper);
    if(res != FMP4_OK) return -1;

    if(userdata->emsg != NULL) {
        userdata->emsg->presentation_time = userdata->track->base_media_decode_time;
    }

    /* expire any old emsgs */
    expire_emsgs(userdata);

    return 0;
}

static int plugin_flush(void* ud, const segment_handler* handler) {
    int r;
    plugin_userdata* userdata = (plugin_userdata*)ud;
    if(userdata->track->trun_sample_count > 0) {
        if( (r = plugin_muxer_flush(userdata,handler)) != 0) return r;
    }
    return handler->flush(handler->userdata);
}

static int plugin_submit_packet(void* ud, const packet* packet, const segment_handler* handler) {
    plugin_userdata* userdata = (plugin_userdata*)ud;
    int r;
    fmp4_sample_info info;

    /* see if we need to flush the current segment */
    if(userdata->track->trun_sample_count + packet->duration > userdata->samples_per_segment) {
        if( (r = plugin_muxer_flush(userdata,handler)) != 0) return r;
    }

    fmp4_sample_info_init(&info);
    info.duration = packet->duration;
    info.size = packet->data.len;
    info.flags.is_non_sync = !packet->sync;

    if(fmp4_track_add_sample(userdata->track, packet->data.x, &info) != FMP4_OK) return -1;

    return 0;
}

static size_t plugin_write_init_callback(const void* src, size_t len, void* userdata) {
    const segment_handler* handler = (const segment_handler*)userdata;
    segment s;

    s.type = SEGMENT_TYPE_INIT;
    s.data = src;
    s.len = len;

    return handler->cb(handler->userdata,&s) == 0 ? len : 0;
}

static int plugin_submit_dsi(void* ud, const membuf* data,const segment_handler* handler) {
    plugin_userdata* userdata = (plugin_userdata*)ud;
    if( fmp4_track_set_dsi(userdata->track, data->x, data->len) != FMP4_OK) {
        fprintf(stderr,"[muxer:fmp4] error setting dsi\n");
        return -1;
    }

    return fmp4_mux_write_init(&userdata->mux, plugin_write_init_callback, (void *)handler) == FMP4_OK ? 0 : -1;
}

static int plugin_outinfo_cb(void* ud, const outputinfo* info) {
    plugin_userdata* userdata = (plugin_userdata*)ud;

    userdata->segment_length = info->segment_length;
    if(userdata->segment_length == 0) userdata->segment_length = 1;
    return 0;
}


static int plugin_open(void* ud, const muxerconfig *config, const outputconfig_handler* handler) {
    int r;
    fmp4_sample_info info;
    plugin_userdata* userdata = (plugin_userdata*)ud;

    outputconfig oconfig = OUTPUTCONFIG_ZERO;
    muxerinfo minfo = MUXERINFO_ZERO;

    userdata->track->stream_type = FMP4_STREAM_TYPE_AUDIO;

    switch(config->type) {
        case CODEC_TYPE_USAC: /* fall-through */
        case CODEC_TYPE_AAC: {
            userdata->track->codec = FMP4_CODEC_MP4A;
            userdata->track->object_type = FMP4_OBJECT_TYPE_AAC;
            break;
        }
        case CODEC_TYPE_ALAC: {
            userdata->track->codec = FMP4_CODEC_ALAC;
            break;
        }
        case CODEC_TYPE_FLAC: {
            userdata->track->codec = FMP4_CODEC_FLAC;
            break;
        }
        default: {
            fprintf(stderr,"[muxer:fmp4] unsupported codec\n");
            return -1;
        }
    }

    fmp4_track_set_language(userdata->track,"und");
    userdata->track->time_scale = config->sample_rate;
    userdata->track->info.audio.channels = config->channels;

    fmp4_sample_info_init(&info);
    info.duration = config->frame_len;
    info.flags.is_non_sync = config->sync_flag == 0;

    fmp4_track_set_default_sample_info(userdata->track, &info);

    /* let the output plugin know what we're doing */
    oconfig.init_ext   = &ext_mp4;
    oconfig.media_ext  = &ext_m4s;
    oconfig.init_mime  = &mime_mp4;
    oconfig.media_mime = &mime_m4s;
    oconfig.time_base  = config->sample_rate;

    oconfig.info.userdata = userdata;
    oconfig.info.submit = plugin_outinfo_cb;

    if( (r = handler->submit(handler->userdata, &oconfig)) != 0) return r;

    /* now we have the frame length and a segment length set, tell
     * the encoder our tune in period */
    minfo.packets_per_segment = userdata->segment_length * config->sample_rate / config->frame_len;
    userdata->samples_per_segment = minfo.packets_per_segment * config->frame_len;
    fprintf(stderr,"[muxer:fmp4]: samples_per_segment = %u\n", userdata->samples_per_segment);
    fprintf(stderr,"[muxer:fmp4]: packets_per_segment = %u\n", minfo.packets_per_segment);

    return config->info.submit(config->info.userdata, &minfo);

}


static int plugin_init(void) {
    int r;
    map_init(&measurement_method_keys);
    map_init(&measurement_system_keys);
    map_init(&reliability_keys);

    /* measurement systems */
    if( (r = map_add_cstr_u8(&measurement_system_keys, "unknown",0)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_system_keys, "other",0)) != 0) return r;

    if( (r = map_add_cstr_u8(&measurement_system_keys, "ebu-r128",1)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_system_keys, "ebu r128",1)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_system_keys, "ebur128",1)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_system_keys, "r128",1)) != 0) return r;

    if( (r = map_add_cstr_u8(&measurement_system_keys, "itu-r bs.1770-3",2)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_system_keys, "itu bs.1770-3",2)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_system_keys, "bs.1770-3",2)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_system_keys, "bs.1770",2)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_system_keys, "bs1770",2)) != 0) return r;

    if( (r = map_add_cstr_u8(&measurement_system_keys, "itu-r bs.1770-3 pre",3)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_system_keys, "itu-r bs.1770-3pre",3)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_system_keys, "itu bs.1770-3 pre",3)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_system_keys, "itu bs.1770-3pre",3)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_system_keys, "bs.1770-3 pre",3)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_system_keys, "bs.1770-3pre",3)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_system_keys, "bs.1770 pre",3)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_system_keys, "bs.1770pre",3)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_system_keys, "bs1770 pre",3)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_system_keys, "bs1770pre",3)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_system_keys, "user",4)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_system_keys, "expert",5)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_system_keys, "panel",5)) != 0) return r;

    /* method definitions */
    if( (r = map_add_cstr_u8(&measurement_method_keys, "unknown",0)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "other",0)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "program loudness",1)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "program",1)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "anchor loudness",2)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "anchor",2)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "maximum of range",3)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "maximum of the range",3)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "maxrange",3)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "maximum momentary loudness",4)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "max momentary loudness",4)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "maximum momentary",4)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "max momentary",4)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "maximum short-term loudness",5)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "maximum short-term",5)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "max short-term loudness",5)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "max short-term",5)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "max short loudness",5)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "max short",5)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "loudness range",6)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "range",6)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "sound pressure level",7)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "sound pressure",7)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "pressure level",7)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "pressure",7)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "production room type index",8)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "production room index",8)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "room index",8)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "index",8)) != 0) return r;
    if( (r = map_add_cstr_u8(&measurement_method_keys, "room",8)) != 0) return r;

    /* reliability values */
    if( (r = map_add_cstr_u8(&reliability_keys, "unknown",0)) != 0) return r;
    if( (r = map_add_cstr_u8(&reliability_keys, "other",0)) != 0) return r;
    if( (r = map_add_cstr_u8(&reliability_keys, "unverified",1)) != 0) return r;
    if( (r = map_add_cstr_u8(&reliability_keys, "reported",1)) != 0) return r;
    if( (r = map_add_cstr_u8(&reliability_keys, "reported but unverified",1)) != 0) return r;
    if( (r = map_add_cstr_u8(&reliability_keys, "ceiling",2)) != 0) return r;
    if( (r = map_add_cstr_u8(&reliability_keys, "accurate",3)) != 0) return r;
    if( (r = map_add_cstr_u8(&reliability_keys, "measured",3)) != 0) return r;
    if( (r = map_add_cstr_u8(&reliability_keys, "measured and accurate",3)) != 0) return r;
    return 0;
}

static void plugin_deinit(void) {
    map_free(&measurement_system_keys);
    map_free(&measurement_method_keys);
    map_free(&reliability_keys);
    return;
}

const muxer_plugin muxer_plugin_fmp4 = {
    { .a = 0, .len = 4, .x = (uint8_t*)"fmp4" },
    plugin_init,
    plugin_deinit,
    plugin_create,
    plugin_config,
    plugin_open,
    plugin_close,
    plugin_submit_dsi,
    plugin_submit_packet,
    plugin_submit_tags,
    plugin_flush,
};
