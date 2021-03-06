/*
 * Copyright (c) 2016, Angel Marin
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *  3. Neither the name of the copyright holder nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "rpirtspd.h"

#include "gstsrc.h"

#include "alsasrc.h"

static GHashTable *hash_media = NULL;
static GHashTable *hash_opts = NULL;
static const gchar *rpicam_params[] = { "camera-number", "bitrate", "sensor-mode", "quantisation-parameter", "do-timestamp", "rotation", "hflip", "vflip", "roi-x", "roi-y", "roi-w", "roi-h", "sharpness", "contrast", "brightness", "saturation", "iso", "inline-headers", "shutter-speed", "drc", "vstab", "video-stabilisation", "exposure-mode", "exposure-compensation", "metering-mode", "image-effect", "awb-mode", "awb-gain-red", "awb-gain-blue", "keyframe-interval", "intra-refresh-type", "annotation-mode", "annotation-text", "annotation-text-size", "annotation-text-colour", "annotation-text-bg-colour", NULL };
static const gchar *videocaps_params[] = { "caps", NULL };
static const gchar *audio_params[] = { "device", NULL };
static const gchar *audioq_params[] = { "flush-on-eos", "leaky",  "max-size-buffers", "max-size-bytes", "max-size-time", "min-threshold-buffers", "min-threshold-bytes", "min-threshold-time", "silent", NULL };

static void media_configure (GstRTSPMediaFactory * factory, GstRTSPMedia * media, gpointer user_data) {
  if ( hash_media == NULL ) {
    return;
  }
  g_hash_table_insert(hash_media, user_data, media);
  if ( rs_args__control_persist ) {
    server_gstsrc_reconfigure(user_data, media);
  }
  return;
}

/* this timeout is periodically run to clean up the expired sessions from the
 * pool. This needs to be run explicitly currently but might be done
 * automatically as part of the mainloop. */
static gboolean session_pool_cleanup (GstRTSPServer * server, gboolean ignored) {
  GstRTSPSessionPool *pool;

  pool = gst_rtsp_server_get_session_pool (server);
  gst_rtsp_session_pool_cleanup (pool);
  g_object_unref (pool);

  return TRUE;
}

static gboolean stream_test( const gchar *pipeline_description ) {
  GError *error = NULL;
  gst_parse_launch (pipeline_description, &error);
  if ( error ) {
    if ( !rs_args__out_quiet ) {
      g_print ("Pipeline failure [%s]\n", pipeline_description);
    }
    g_error_free(error);
    return FALSE;
  }
  return TRUE;
}

static gchar * stream_pipeline( gchar *pipeline_video, gchar *pipeline_audio ) {
  gchar *pipeline = g_strdup_printf("( %s %s )", pipeline_video ?pipeline_video : "", pipeline_audio ? pipeline_audio : "");
  if ( pipeline_video ) {
    g_free(pipeline_video);
  }
  if ( pipeline_audio ) {
    g_free(pipeline_audio);
  }
  return pipeline;
}

static gchar * stream_pipeline_video( void ) {
  gchar *pipeline = g_strdup_printf("%s name=videosrc1 %s !"
    " capsfilter caps=video/x-h264,width=%d,height=%d,framerate=%d/1,profile=%s name=videocaps1 !"
    " queue leaky=downstream max-size-time=0 max-size-buffers=2 !"
    " h264parse ! rtph264pay config-interval=1 pt=96 name=pay0",
      rs_args__video_source ? rs_args__video_source : "rpicamsrc",
      rs_args__video_args ? rs_args__video_args : "bitrate=1000000",
      rs_args__video_width, rs_args__video_height, rs_args__video_frm,
      rs_args__video_profile ? rs_args__video_profile : "baseline");
  return pipeline;
}

static gchar * stream_pipeline_audio( gchar **audio_devices, gint dpos, gint ppos ) {
  if ( !audio_devices ) {
    return NULL;
  }
  gchar *device = audio_devices[dpos];
  if ( device ) {
    device = g_strdup_printf("device=plughw:%s", device);
  }
  if ( !device && !rs_args__audio_args ) {
    return NULL;
  }
  gchar *pipeline = g_strdup_printf("alsasrc name=audiosrc1 %s !"
    " queue name=qaudio1 leaky=no max-size-time=4500000000 max-size-buffers=0 min-threshold-time=%d !"
    " audio/x-raw,channels=%d,rate=%d ! audioresample ! audioconvert ! %s bitrate=%d ! %s name=pay%d pt=97",
      ppos==1 && rs_args__audio_args ? rs_args__audio_args : device,
      rs_args__audio_delay*1000000,
      rs_args__audio_channels, rs_args__audio_clockrate,
      rs_args__audio_compress ? "voaacenc" : "alawenc",
      rs_args__audio_bitrate,
      rs_args__audio_compress ? "rtpmp4apay" : "rtppcmapay",
       ppos );
  if ( device ) {
    g_free(device);
  }
  return pipeline;
}

static void set_streams_audio( GstRTSPMountPoints *mounts, gchar **audio_devices ) {
  int i;
  if ( !audio_devices ) {
    return;
  }
  for ( i=0; audio_devices[i]!=NULL; i++ ) {
    gchar *stream_name;
    gchar *mount_path;
    gchar *pipeline = stream_pipeline_audio(audio_devices, i, 0);
    if ( !pipeline ) {
      continue;
    }
    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new ();
    stream_name = g_strdup_printf("audio%d", i+1);

    gst_rtsp_media_factory_set_shared(factory, TRUE);
    g_signal_connect (factory, "media-configure", (GCallback)media_configure, stream_name );
    if ( rs_args__out_verbose ) {
      g_print("set_streams_audio[%s]: Pipeline [%s]\n", stream_name, pipeline);
    }
    mount_path = g_strdup_printf("/audio%d", i+1);
    gst_rtsp_media_factory_set_launch (factory, pipeline);
    gst_rtsp_mount_points_add_factory (mounts, mount_path, factory);
    if ( !rs_args__out_quiet ) {
      g_print ("[rtsp://127.0.0.1:8554%s] Audio stream [alsa hw:%s]\n", mount_path, audio_devices[i]);
    }
  }
}

static void set_stream_main( GstRTSPMountPoints *mounts, gchar **audio_devices ) {
  gchar *pipeline;
  GstRTSPMediaFactory *factory;
  factory = gst_rtsp_media_factory_new ();
  gst_rtsp_media_factory_set_shared(factory, TRUE);
  g_signal_connect (factory, "media-configure", (GCallback)media_configure, "main" );
  pipeline = stream_pipeline(stream_pipeline_video(), stream_pipeline_audio(audio_devices, 0, 1));
  if ( rs_args__out_verbose ) {
    g_print("set_stream_main: Pipeline [%s]\n", pipeline);
  }
  gst_rtsp_media_factory_set_launch (factory, pipeline);
  gst_rtsp_mount_points_add_factory (mounts, "/main", factory);
  if ( !rs_args__out_quiet ) {
    g_print ("[rtsp://127.0.0.1:8554/main] Main stream (audio+video)\n");
  }
}

static void set_stream_test( GstRTSPMountPoints *mounts ) {
  GstRTSPMediaFactory *factory;
  /* make a media factory for a test stream. The default media factory can use
   * gst-launch syntax to create pipelines.
   * any launch line works as long as it contains elements named pay%d. Each
   * element with pay%d names will be a stream */
  factory = gst_rtsp_media_factory_new ();
  /* Media from a factory can be shared by setting the shared flag with
   * gst_rtsp_media_factory_set_shared(). When a factory is shared,
   * gst_rtsp_media_factory_construct() will return the same GstRTSPMedia when
   * the url matches. */
  gst_rtsp_media_factory_set_shared(factory, TRUE);
  /* any launch line works as long as it contains elements named pay%d. Each
   * element with pay%d names will be a stream */
  gst_rtsp_media_factory_set_launch (factory, "( "
      "videotestsrc ! video/x-raw,width=352,height=288,framerate=15/1 ! "
      "x264enc ! rtph264pay name=pay0 pt=96 "
      "audiotestsrc ! audio/x-raw,rate=8000 ! "
      "alawenc ! rtppcmapay name=pay1 pt=97 " ")");
  /* attach the factory to the url */
  gst_rtsp_mount_points_add_factory (mounts, "/test", factory);
  if ( !rs_args__out_quiet ) {
    g_print ("[rtsp://127.0.0.1:8554/test] Test stream\n");
  }
  return;
}

static void set_stream_video( GstRTSPMountPoints *mounts ) {
  gchar *pipeline;
  GstRTSPMediaFactory *factory;
  pipeline = stream_pipeline(stream_pipeline_video(), NULL);
  if ( pipeline==NULL ) {
    return;
  }
  if ( rs_args__out_verbose ) {
    g_print("set_stream_video: Pipeline [%s]\n", pipeline);
  }
  factory = gst_rtsp_media_factory_new ();
  gst_rtsp_media_factory_set_shared(factory, TRUE);
  g_signal_connect (factory, "media-configure", (GCallback)media_configure, "video" );
  gst_rtsp_media_factory_set_launch (factory, pipeline);
  gst_rtsp_mount_points_add_factory (mounts, "/video", factory);
  if ( !rs_args__out_quiet ) {
    g_print ("[rtsp://127.0.0.1:8554/video] Video only stream\n");
  }
  return;
}

gboolean server_gstsrc_hasparam( const gchar * const *strv, gchar *param ) {
  for( ; *strv != NULL; strv++ ) {
    if ( g_str_equal (param, *strv) ) {
      return TRUE;
    }
  }
  return FALSE;
}

gboolean server_gstsrc_configure( gchar *params ) {
  GstRTSPMedia *media_video = NULL;
  GstElement *pipeline = NULL;
  GstElement *gstelement = NULL;
  gchar **tokens1 = NULL;
  gchar **tokens2 = NULL;
  guint i;
  if ( params == NULL ) {
    g_warning("server_gstsrc_configure: No valid params");
    return FALSE;
  }
  tokens1 = g_strsplit(params, " ", -1);
  for ( i = 0; tokens1 && tokens1[i]; i++ ) {
    if ( G_IS_OBJECT(gstelement) ) {
      g_object_unref(gstelement);
    }
    if ( g_ascii_strcasecmp(tokens1[i], "reset")==0 ) {
      if ( rs_args__control_persist ) {
        g_hash_table_remove_all(hash_opts);
      }
      continue;
    }
    gstelement = NULL;
    if ( g_strrstr(tokens1[i], "=")==NULL ) {
      if ( G_IS_OBJECT(pipeline) ) {
        g_object_unref(pipeline);
      }
      pipeline = NULL;
      media_video = g_hash_table_lookup(hash_media, tokens1[i]);
      if ( G_IS_OBJECT(media_video) ) {
        pipeline = gst_rtsp_media_get_element(media_video);
        if ( ! G_IS_OBJECT(pipeline) ) {
          g_warning("server_gstsrc_configure[%s] Root element not found", tokens1[i]);
        }
      } else {
        g_debug("server_gstsrc_configure[%s] No active media found", tokens1[i]);
      }
      continue;
    }
    g_debug("server_gstsrc_configure[%s]\n", tokens1[i]);
    tokens2 = g_strsplit(tokens1[i], "=", 2);
    if ( g_strv_length(tokens2)!=2 ) {
      g_strfreev(tokens2);
      continue;
    }
    gchar *token = g_strdup(tokens2[1]);
    if ( g_str_has_prefix(token, "\"") ) {
      if ( strlen(token)==1 || !g_str_has_suffix(token, "\"") ) {
        for ( i++; tokens1 && tokens1[i]; i++ ) {
          gchar *token2 = g_strjoin(" ", token, tokens1[i], NULL);
          g_free(token);
          token = token2;
          if ( g_str_has_suffix(token, "\"") ) {
            break;
          }
        }
      }
    }
    if ( g_str_has_prefix(token, "\"") ) {
      memmove(token, token+1, strlen(token));
    }
    if ( g_str_has_suffix(token, "\"") ) {
      token[strlen(token) - 1] = '\0';
    }
    if ( server_gstsrc_hasparam(rpicam_params, tokens2[0]) ) {
      if ( G_IS_OBJECT(pipeline) ) {
        gstelement = gst_bin_get_by_name(GST_BIN(pipeline), "videosrc1");
      }
    } else if ( server_gstsrc_hasparam(videocaps_params, tokens2[0]) ) {
      if ( G_IS_OBJECT(pipeline) ) {
        gstelement = gst_bin_get_by_name(GST_BIN(pipeline), "videocaps1");
      }
    } else if ( server_gstsrc_hasparam(audio_params, tokens2[0]) ) {
      if ( G_IS_OBJECT(pipeline) ) {
        gstelement = gst_bin_get_by_name(GST_BIN(pipeline), "audiosrc1");
      }
    } else if ( server_gstsrc_hasparam(audioq_params, tokens2[0]) ) {
      if ( G_IS_OBJECT(pipeline) ) {
        gstelement = gst_bin_get_by_name(GST_BIN(pipeline), "qaudio1");
      }
    } else {
      g_warning("server_gstsrc_configure[%s][%s] Parameter not found", tokens2[0], token);
      g_strfreev(tokens2);
      g_free(token);
      continue;
    }
    g_debug("server_gstsrc_configure[%s][%s]", tokens2[0], token);
    if ( rs_args__control_persist ) {
      g_hash_table_insert(hash_opts, g_strdup(tokens2[0]), g_strdup(token));
    }
    if ( G_IS_OBJECT(gstelement) ) {
      gst_util_set_object_arg(G_OBJECT(gstelement), tokens2[0], token);
    } else {
      g_debug("server_gstsrc_configure[%s][%s] element not found", tokens2[0], token);
    }
    g_free(token);
    g_strfreev(tokens2);
  }
  if ( G_IS_OBJECT(gstelement) ) {
    g_object_unref(gstelement);
  }
  if ( G_IS_OBJECT(pipeline) ) {
    g_object_unref(pipeline);
  }
  g_strfreev(tokens1);
  return TRUE;
}
gboolean server_gstsrc_reconfigure ( const gchar *stream_name, GstRTSPMedia *media ) {
  GstElement *pipeline = NULL;
  GstElement *gstelement = NULL;
  GHashTableIter iter;
  gpointer key, value;
  pipeline = gst_rtsp_media_get_element(media);
  if ( ! G_IS_OBJECT(pipeline) ) {
    g_warning("server_gstsrc_reconfigure[%s] Root element not found", stream_name);
    return FALSE;
  }
  g_hash_table_iter_init(&iter, hash_opts);
  while ( g_hash_table_iter_next (&iter, &key, &value) ) {
    gstelement = NULL;
    if ( server_gstsrc_hasparam(rpicam_params, key) ) {
      gstelement = gst_bin_get_by_name(GST_BIN(pipeline), "videosrc1");
    } else if ( server_gstsrc_hasparam(videocaps_params, key) ) {
      gstelement = gst_bin_get_by_name(GST_BIN(pipeline), "videocaps1");
    } else if ( server_gstsrc_hasparam(audio_params, key) ) {
      gstelement = gst_bin_get_by_name(GST_BIN(pipeline), "audiosrc1");
    } else if ( server_gstsrc_hasparam(audioq_params, key) ) {
      gstelement = gst_bin_get_by_name(GST_BIN(pipeline), "qaudio1");
    }
    if ( G_IS_OBJECT(gstelement) ) {
      g_debug("server_gstsrc_reconfigure[%s][%s]\n", stream_name, (gchar *)key);
      gst_util_set_object_arg(G_OBJECT(gstelement), key, value);
    }
  }
  return TRUE;
}

gint server_gstsrc_startgst_init (int *argc, char **argv[]) {
  GstRTSPServer *server;
  GstRTSPMountPoints *mounts;
  gchar **audio_devices;

  gst_init (argc, argv);

  if ( !stream_test("(rpicamsrc)") || !stream_test("(alsasrc)") ) {
    if ( !rs_args__mode_test ) {
      goto failed;
    }
  }

  /* create active media hash table */
  hash_media = g_hash_table_new(g_str_hash, g_str_equal);

  /* create persist options hash table */
  if ( rs_args__control_persist ) {
    hash_opts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  }

  /* Get audio devices */
  audio_devices = audio_alsasrc_device_list();

  /* create a server instance */
  server = gst_rtsp_server_new ();

  /* get the mount points for this server, every server has a default object
   * that be used to map uri mount points to media factories */
  mounts = gst_rtsp_server_get_mount_points (server);

  /* Test stream (video+audio) */
  set_stream_test(mounts);

  /* Main stream (video+audio) */
  set_stream_main(mounts, audio_devices);

  /* Video stream */
  set_stream_video(mounts);

  /* Audio streams */
  set_streams_audio(mounts, audio_devices);

  /* don't need the ref to the mapper anymore */
  g_object_unref (mounts);

  /* don't need the audio devices anymore */
  g_strfreev(audio_devices);

  if ( rs_args__listen_rtsp ) {
    if ( rs_args__bind_address ) {
      gst_rtsp_server_set_address(server, rs_args__bind_address);
    }
    if ( rs_args__bind_port ) {
      gst_rtsp_server_set_service(server, rs_args__bind_port);
    }
    /* attach the server to the default maincontext */
    if (gst_rtsp_server_attach (server, NULL) == 0)
      goto failed;

    /* add a timeout for the session cleanup */
    g_timeout_add_seconds (2, (GSourceFunc) session_pool_cleanup, server);

    if ( !rs_args__out_quiet ) {
      g_print ("Server streams ready for clients "
        "[%s][%s]\n", gst_rtsp_server_get_address(server), gst_rtsp_server_get_service(server));
    }
  }
  return 0;
  /* ERRORS */
failed:
  {
    if ( !rs_args__out_quiet ) {
      g_print ("failed to start rtsp server\n");
    }
    return 1;
  }
}
