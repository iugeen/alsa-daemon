/****************************************
 * a2dp-alsa.c
 * Enable A2DP sink and source on ALSA devices using bluez DBus API.
 * In short - it enables remote devices to send sound to the computer (sink)
 * and enables the computer (source) to send sound to bluetooth speakers.
 *
 * For bluez 5.x.
 *
 * Copyright (C) James Budiono 2013
 * License: GNU GPL Version 3 or later
 * Version 2: April 2014
 ****************************************/
// std includes
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <fcntl.h>
#include <dbus/dbus.h>
#include <errno.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <poll.h>
#include <alsa/asoundlib.h>
#include <sbc/sbc.h>
#include "uthash.h"
#include "time-smoother.h"
#include "parser.h"
#include "device.h"

// our own defines
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

//#define DEBUG
#ifdef DEBUG
#define debug_print(...) (fprintf (stderr, __VA_ARGS__))
#else
#define debug_print(...)
#endif

#define HISTORY_MAX 64

// bluez specific defines & includes
#define GET_MANAGED_OBJECTS "GetManagedObjects"
#define DEFAULT_ADAPTER "DefaultAdapter"

// sink and source uuid and endpoints
#define A2DP_SINK_UUID		   "0000110b-0000-1000-8000-00805f9b34fb"
#define A2DP_SINK_ENDPOINT   "/MediaEndpoint/A2DPSink1" // bt --> alsa (sink for bt)
#define A2DP_SINK_ENDPOINT_SECOND   "/MediaEndpoint/A2DPSink2" // bt --> alsa (sink for bt)
#define A2DP_SOURCE_UUID	   "0000110a-0000-1000-8000-00805f9b34fb"
#define A2DP_SOURCE_ENDPOINT "/MediaEndpoint/A2DPSource1" // alsa --> bt (source for bt)


///==================================================================

#define HFP_HS_UUID             "0000111e-0000-1000-8000-00805f9b34fb"
#define HFP_AG_UUID             "0000111f-0000-1000-8000-00805f9b34fb"

#define HFP_AG_ENDPOINT "/MediaEndpoint/HFPAG"
#define HFP_HS_ENDPOINT "/MediaEndpoint/HFPHS"

#define OFONO_VOICEMGR_SIGNAL_IF		  "org.ofono.VoiceCallManager"
#define OFONO_CALL_ADDED_SIG			    "CallAdded"
#define OFONO_CALL_REMOVED_SIG			  "CallRemoved"
#define OFONO_VOICE_SIGNAL_IF			    "org.ofono.VoiceCall"
#define OFONO_VOICE_PROP_CHANGED_SIG	"PropertyChanged"

///===================================================================

#define pa_streq(a,b) (!strcmp((a),(b)))
#define PCM_DEVICE        "A2DP_playback_0"
#define PCM_DEVICE_SECOND "A2DP_playback_1"

void *io_thread_run(void *ptr);
void *stream_bt_input(void *ptr);
void *pa_push(void *ptr);
void *a2dp_push(void *ptr);
void *stream_bt_output(void *ptr);
//void *stream_bt_outputTEST(void *ptr);
void io_thread_set_command (io_thread_tcb_s *data, int command);
io_thread_tcb_s *create_io_thread();
void destroy_io_thread(io_thread_tcb_s *p);
int transport_acquire (DBusConnection *conn, char *transport_path, int *fd, int *read_mtu, int *write_mtu);
int transport_release (DBusConnection *conn, char *transport_path, void *ptr);

// globals
int quit=0;        // when set to 1, program terminates
int run_once  = 0; // only run output once, then exit
static int devNumber = 0; // number of connected devices
int threadId = 0;

//////////////////////////////// DBUS HELPERS ////////////////////////////////

/*****************//**
 * Handle dbus error and clear error message block
 *
 * @param [in] The error object
 * @param [in] function where the error happens
 * @param [in] line number where the error happens
 * @returns TRUE if successful, FALSE otherwise
 ********************/
int handle_dbus_error (DBusError *err, const char *func, int line)
{
  if (dbus_error_is_set (err))
  {
    fprintf(stderr, "DBus error %s at %u: %s\n", func, line, err->message);
    dbus_error_free(err);
    return 1;
  }
  return 0;
}

/*****************//**
 * Connect to the system message bus
 *
 * @param [out] connection object, if function is successful, other wise it is unchanged.
 * @returns TRUE if successful, FALSE otherwise
 ********************/
int get_system_bus(DBusConnection **system_bus)
{
  DBusError err;
  DBusConnection* conn;

  dbus_error_init(&err);
  conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
  handle_dbus_error (&err, __FUNCTION__, __LINE__);
  if (NULL == conn) return 0;

  *system_bus = conn;
  debug_print("Name %s\n", dbus_bus_get_unique_name (conn));
  return 1;
}

/*****************//**
 * Add a dict entry of a variant of simple types
 *
 * @param [in] array iter to add to
 * @param [in] key
 * @param [in] type (must be simple types)
 * @param [in] value
 ********************/
void util_add_dict_variant_entry (DBusMessageIter *iter, char *key, int type, void *value)
{
  DBusMessageIter dict, variant;
  dbus_message_iter_open_container (iter, DBUS_TYPE_DICT_ENTRY, NULL, &dict);
  dbus_message_iter_append_basic (&dict, DBUS_TYPE_STRING, &key);


  dbus_message_iter_open_container (&dict, DBUS_TYPE_VARIANT, (char *)&type, &variant);
  dbus_message_iter_append_basic (&variant, type, &value);
  dbus_message_iter_close_container (&dict, &variant);
  dbus_message_iter_close_container (iter, &dict);
}

/*****************//**
     * Add a dict entry of an array of of simple types
     *
     * @param [in] array iter to add to
     * @param [in] key
     * @param [in] type (must be simple types)
     * @param [in] pointer to the array
     * @param [in] number of elements (not size in bytes!)
     ********************/
void util_add_dict_array_entry (DBusMessageIter *iter, char *key, int type, void *buf, int elements)
{
  DBusMessageIter dict, variant, array;
  char array_type[5] = "a";
  strncat (array_type, (char*)&type, sizeof(array_type));

  dbus_message_iter_open_container (iter, DBUS_TYPE_DICT_ENTRY, NULL, &dict);
  dbus_message_iter_append_basic (&dict, DBUS_TYPE_STRING, &key);
  dbus_message_iter_open_container (&dict, DBUS_TYPE_VARIANT, array_type, &variant);
  dbus_message_iter_open_container (&variant, DBUS_TYPE_ARRAY, (char *)&type, &array);
  dbus_message_iter_append_fixed_array (&array, type, &buf, elements);
  dbus_message_iter_close_container (&variant, &array);
  dbus_message_iter_close_container (&dict, &variant);
  dbus_message_iter_close_container (iter, &dict);
}



//////////////////////////////// BLUEZ AUDIO/MEDIA HELPERS ////////////////////////////////

/*****************//**
     * Register our "endpoint" handler to bluez audio system.
     * As part of its job, it returns supported codecs and codec parameters, as well
     * as what functions are we doing here (A2DP source, A2DP sink, HFP, etc - for
     * this program it will be A2DP sink).
     *
     * @param [in] system bus connection
     * @param [in] bluetooth object to register to
     * @returns TRUE means ok, FALSE means something is wrong
     ********************/
int media_register_endpoint(DBusConnection* conn, char *bt_object, char *endpoint, char *uuid)
{
  DBusMessage *msg, *reply;
  DBusMessageIter iter, iterarray;
  DBusError err;

  a2dp_sbc_t capabilities;
  capabilities.channel_mode = BT_A2DP_CHANNEL_MODE_MONO | BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL |
      BT_A2DP_CHANNEL_MODE_STEREO | BT_A2DP_CHANNEL_MODE_JOINT_STEREO;
  capabilities.frequency = BT_SBC_SAMPLING_FREQ_16000 | BT_SBC_SAMPLING_FREQ_32000 |
      BT_SBC_SAMPLING_FREQ_44100 | BT_SBC_SAMPLING_FREQ_48000;
  capabilities.allocation_method = BT_A2DP_ALLOCATION_SNR | BT_A2DP_ALLOCATION_LOUDNESS;
  capabilities.subbands = BT_A2DP_SUBBANDS_4 | BT_A2DP_SUBBANDS_8;
  capabilities.block_length = BT_A2DP_BLOCK_LENGTH_4 | BT_A2DP_BLOCK_LENGTH_8 |
      BT_A2DP_BLOCK_LENGTH_12 | BT_A2DP_BLOCK_LENGTH_16;
  capabilities.min_bitpool = MIN_BITPOOL;
  capabilities.max_bitpool = MAX_BITPOOL;

  dbus_error_init(&err);
  msg = dbus_message_new_method_call("org.bluez",
                                     bt_object,			  // object to call on
                                     "org.bluez.Media1",	  // interface to call on
                                     "RegisterEndpoint");  // method name

  //build the parameters
  dbus_message_iter_init_append (msg, &iter);

  //first param - object path
  dbus_message_iter_append_basic (&iter, DBUS_TYPE_OBJECT_PATH, &endpoint);
  //second param - properties
  dbus_message_iter_open_container (&iter, DBUS_TYPE_ARRAY, "{sv}", &iterarray);
  util_add_dict_variant_entry (&iterarray, "UUID", DBUS_TYPE_STRING, uuid);
  util_add_dict_variant_entry (&iterarray, "Codec", DBUS_TYPE_BYTE, A2DP_CODEC_SBC);
  util_add_dict_array_entry (&iterarray, "Capabilities", DBUS_TYPE_BYTE, &capabilities, sizeof (capabilities));
  dbus_message_iter_close_container (&iter, &iterarray);

  //char *buf; int buflen; dbus_message_marshal (msg, &buf, &buflen); // write (1, buf, buflen); return 0;

  //make the call
  reply = dbus_connection_send_with_reply_and_block (conn, msg, -1, &err);
  handle_dbus_error (&err, __FUNCTION__, __LINE__);
  if (!reply)
  {
    fprintf(stderr, "Reply Null\n");
    return 0;
  }

  dbus_message_unref(msg);
  dbus_message_unref(reply);
  return 1;
}

int phone_register_endpoint(DBusConnection* conn, char *bt_object, char *endpoint, char *uuid)
{
  DBusMessage *msg, *reply;
  DBusMessageIter iter, iterarray;
  DBusError err;

  debug_print ("phone_register_endpoint\n");

  a2dp_sbc_t capabilities;
  capabilities.channel_mode = BT_A2DP_CHANNEL_MODE_MONO | BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL |
      BT_A2DP_CHANNEL_MODE_STEREO | BT_A2DP_CHANNEL_MODE_JOINT_STEREO;
  capabilities.frequency = BT_SBC_SAMPLING_FREQ_16000 | BT_SBC_SAMPLING_FREQ_32000 |
      BT_SBC_SAMPLING_FREQ_44100 | BT_SBC_SAMPLING_FREQ_48000;
  capabilities.allocation_method = BT_A2DP_ALLOCATION_SNR | BT_A2DP_ALLOCATION_LOUDNESS;
  capabilities.subbands = BT_A2DP_SUBBANDS_4 | BT_A2DP_SUBBANDS_8;
  capabilities.block_length = BT_A2DP_BLOCK_LENGTH_4 | BT_A2DP_BLOCK_LENGTH_8 |
      BT_A2DP_BLOCK_LENGTH_12 | BT_A2DP_BLOCK_LENGTH_16;
  capabilities.min_bitpool = MIN_BITPOOL;
  capabilities.max_bitpool = MAX_BITPOOL;

  dbus_error_init(&err);
  msg = dbus_message_new_method_call("org.bluez",
                                     bt_object,			  // object to call on
                                     "org.bluez.Media1",	  // interface to call on
                                     "RegisterEndpoint");  // method name

  //build the parameters
  dbus_message_iter_init_append (msg, &iter);

  //first param - object path
  dbus_message_iter_append_basic (&iter, DBUS_TYPE_OBJECT_PATH, &endpoint);
  //second param - properties
  dbus_message_iter_open_container (&iter, DBUS_TYPE_ARRAY, "{sv}", &iterarray);
  util_add_dict_variant_entry (&iterarray, "UUID", DBUS_TYPE_STRING, uuid);
  util_add_dict_variant_entry (&iterarray, "Codec", DBUS_TYPE_BYTE, HFP_AUDIO_CODEC_CVSD);
  util_add_dict_array_entry (&iterarray, "Capabilities", DBUS_TYPE_BYTE, &capabilities, sizeof (capabilities));
  dbus_message_iter_close_container (&iter, &iterarray);



  reply = dbus_connection_send_with_reply_and_block (conn, msg, -1, &err);
  handle_dbus_error (&err, __FUNCTION__, __LINE__);
  if (!reply)
  {
    fprintf(stderr, "Reply Null\n");
    return 0;
  }

  dbus_message_unref(msg);
  dbus_message_unref(reply);
  return 1;
}


//static void register_endpoint(pa_bluetooth_discovery *y, const char *path, const char *endpoint, const char *uuid) {
int new_register_endpoint( DBusConnection* conn, char *path, char *endpoint, char *uuid)
{
  DBusMessage *msg;
  DBusMessage  *reply;
  DBusMessageIter i, d;
  uint8_t codec = 0;
  DBusError err;

  debug_print("Registering %s on adapter %s.", endpoint, path);

  dbus_error_init(&err);
  msg = dbus_message_new_method_call("org.bluez", path, "org.bluez.Media1", "RegisterEndpoint");
  dbus_message_iter_init_append(msg, &i);
  dbus_message_iter_append_basic(&i, DBUS_TYPE_OBJECT_PATH, &endpoint);

  dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY, DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
                                   DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
                                   &d);

  util_add_dict_variant_entry (&d, "UUID",  DBUS_TYPE_STRING, uuid);

  if (pa_streq(uuid, HFP_AG_UUID) || pa_streq(uuid, HFP_HS_UUID))
  {
    debug_print("<<HFP\n");
    util_add_dict_variant_entry (&d, "Codec", DBUS_TYPE_BYTE, HFP_AUDIO_CODEC_CVSD);
    uint8_t capability = 0;
    //pa_dbus_append_basic_array_variant_dict_entry(&d, "Capabilities", DBUS_TYPE_BYTE, &capability, 1);
    util_add_dict_array_entry (&d, "Capabilities", DBUS_TYPE_BYTE, &capability, 1);
  }
  else
  {
    debug_print("<<A2DP\n");

    util_add_dict_variant_entry (&d, "Codec", DBUS_TYPE_BYTE, A2DP_CODEC_SBC);

    a2dp_sbc_t capabilities;

    capabilities.channel_mode = SBC_CHANNEL_MODE_MONO | SBC_CHANNEL_MODE_DUAL_CHANNEL |
        SBC_CHANNEL_MODE_STEREO | SBC_CHANNEL_MODE_JOINT_STEREO;
    capabilities.frequency = SBC_SAMPLING_FREQ_16000 | SBC_SAMPLING_FREQ_32000 |
        SBC_SAMPLING_FREQ_44100 | SBC_SAMPLING_FREQ_48000;
    capabilities.allocation_method = SBC_ALLOCATION_SNR | SBC_ALLOCATION_LOUDNESS;
    capabilities.subbands = SBC_SUBBANDS_4 | SBC_SUBBANDS_8;
    capabilities.block_length = SBC_BLOCK_LENGTH_4 | SBC_BLOCK_LENGTH_8 |
        SBC_BLOCK_LENGTH_12 | SBC_BLOCK_LENGTH_16;
    capabilities.min_bitpool = MIN_BITPOOL;
    capabilities.max_bitpool = MAX_BITPOOL;

    util_add_dict_array_entry (&d, "Capabilities", DBUS_TYPE_BYTE, &capabilities, sizeof (capabilities));
  }

  dbus_message_iter_close_container(&i, &d);

  //send_and_add_to_pending(y, m, register_endpoint_reply, pa_xstrdup(endpoint));

  reply = dbus_connection_send_with_reply_and_block (conn, msg, -1, &err);
  handle_dbus_error (&err, __FUNCTION__, __LINE__);
  if (!reply)
  {
    fprintf(stderr, "Reply Null\n");
    return 0;
  }

  dbus_message_unref(msg);
  dbus_message_unref(reply);
  return 1;
}


/*****************//**
     * Get the transport (ie, the actual file descriptor) for streaming (ie, read/write)
     * the audio data
     *
     * @param [in] system bus connection
     * @param [in] transport object path (must come from MediaEndpoint1.SetConfiguration)
     * @param [out] file descriptor
     * @param [out] maximum size to read per transaction
     * @param [out] maximum size to write per transaction
     * @returns TRUE if ok, FALSE means something is wrong
     ********************/
int transport_acquire (DBusConnection *conn, char *transport_path, int *fd, int *read_mtu, int *write_mtu)
{
  DBusMessage *msg, *reply;
  DBusMessageIter iter;
  DBusError err;

  debug_print ("BT - acquire %s\n", transport_path);
  dbus_error_init(&err);
  msg = dbus_message_new_method_call("org.bluez",
                                     transport_path,                // object to call on
                                     "org.bluez.MediaTransport1",	  // interface to call on
                                     "Acquire");                    // method name

  //build the parameters
  dbus_message_iter_init_append (msg, &iter);

  //make the call
  reply = dbus_connection_send_with_reply_and_block (conn, msg, -1, &err);
  handle_dbus_error (&err, __FUNCTION__, __LINE__);
  if (!reply)
  {
    fprintf(stderr, "Reply Null - (%s)\n", transport_path);
    return 0;
  }

  //read the reply
  if (!dbus_message_get_args(reply, &err,
                             DBUS_TYPE_UNIX_FD, fd,
                             DBUS_TYPE_UINT16, read_mtu,
                             DBUS_TYPE_UINT16, write_mtu,
                             DBUS_TYPE_INVALID))
  {
    handle_dbus_error (&err, __FUNCTION__, __LINE__);
    return 0;
  }

  //clean up
  dbus_message_unref(msg);
  dbus_message_unref(reply);
  return 1;
}

int ofono_transport_acquire (DBusConnection *conn, char *transport_path, int *fd, int *read_mtu, int *write_mtu) {
  DBusMessage *msg, *reply;
  DBusMessageIter iter;
  DBusError err;

  debug_print ("OFONO - acquire %s\n", transport_path);
  dbus_error_init(&err);
  msg = dbus_message_new_method_call("org.bluez",
                                     transport_path,                // object to call on
                                     "org.bluez.MediaTransport1",	  // interface to call on
                                     "Acquire");                    // method name

  //build the parameters
  dbus_message_iter_init_append (msg, &iter);

  //make the call
  reply = dbus_connection_send_with_reply_and_block (conn, msg, -1, &err);
  handle_dbus_error (&err, __FUNCTION__, __LINE__);
  if (!reply)
  {
    fprintf(stderr, "Reply Null - (%s)\n", transport_path);
    return 0;
  }

  //read the reply
  if (!dbus_message_get_args(reply, &err,
                             DBUS_TYPE_UNIX_FD, fd,
                             DBUS_TYPE_UINT16, read_mtu,
                             DBUS_TYPE_UINT16, write_mtu,
                             DBUS_TYPE_INVALID))
  {
    handle_dbus_error (&err, __FUNCTION__, __LINE__);
    return 0;
  }

  //clean up
  dbus_message_unref(msg);
  dbus_message_unref(reply);
  return 1;
}

/*****************//**
     * Release the transport (ie, the file descriptor)
     * Note: this doesn't need to be called if transport is closed by
     * "MediaEndpoint1.ClearConfiguration". It is only needed if the app wishes to
     * release control of the fd while the stream is still active (e.g - suspend
     * stream, pausing, etc).
     *
     * @param [in] system bus connection
     * @param [in] transport object path (must come from MediaEndpoint1.SetConfiguration)
     ********************/
int transport_release(DBusConnection *conn, char *transport_path, void *ptr)
{

  DBusMessage *msg, *reply;
  DBusMessageIter iter;
  DBusError err;

  debug_print ("\nRRelease %s\n", transport_path);
  dbus_error_init(&err);
  msg = dbus_message_new_method_call("org.bluez",
                                     transport_path,                // object to call on
                                     "org.bluez.MediaTransport1",	  // interface to call on
                                     "Release");                    // method name

  //build the parameters
  dbus_message_iter_init_append (msg, &iter);

  //make the call
  reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
  handle_dbus_error (&err, __FUNCTION__, __LINE__);
  if (!reply)
  {
    fprintf(stderr, "Reply Null\n");
    return 0;
  }

  //clean up
  dbus_message_unref(msg);
  dbus_message_unref(reply);
  return 1;
}

/*****************//**
     * Helper to calculate the optimum bitpool, given the sampling frequency,
     * and number of channels.
     * Taken verbatim from pulseaudio 2.1
     * (which took it from bluez audio - a2dp.c & pcm_bluetooth.c - default_bitpool)
     *
     * @param [in] frequency
     * @param [in] channel mode
     * @returns coded SBC bitpool
     *********************/
static uint8_t a2dp_default_bitpool(uint8_t freq, uint8_t mode) {

  switch (freq) {
  case BT_SBC_SAMPLING_FREQ_16000:
  case BT_SBC_SAMPLING_FREQ_32000:
    return 53;

  case BT_SBC_SAMPLING_FREQ_44100:

    switch (mode) {
    case BT_A2DP_CHANNEL_MODE_MONO:
    case BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL:
      return 31;

    case BT_A2DP_CHANNEL_MODE_STEREO:
    case BT_A2DP_CHANNEL_MODE_JOINT_STEREO:
      return 53;

    default:
      fprintf (stderr, "Invalid channel mode %u", mode);
      return 53;
    }

  case BT_SBC_SAMPLING_FREQ_48000:

    switch (mode) {
    case BT_A2DP_CHANNEL_MODE_MONO:
    case BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL:
      return 29;

    case BT_A2DP_CHANNEL_MODE_STEREO:
    case BT_A2DP_CHANNEL_MODE_JOINT_STEREO:
      return 51;

    default:
      fprintf (stderr, "Invalid channel mode %u", mode);
      return 51;
    }

  default:
    fprintf (stderr, "Invalid sampling freq %u", freq);
    return 53;
  }
}

/*****************//**
     * Helper to setup sbc params from a2dp_sbc_t
     * Modified from pulseaudio 2.1 (which took it from bluez - pcm_bluetooth.c
     * - bluetooth_a2dp_setup)
     *
     * @param [in] sbc codec configuration
     * @param [in] bluez codec capability configuration
     *********************/
void setup_sbc(sbc_t *sbc, a2dp_sbc_t *cap)
{

  switch (cap->frequency)
  {
  case BT_SBC_SAMPLING_FREQ_16000:
    sbc->frequency = SBC_FREQ_16000;
    break;
  case BT_SBC_SAMPLING_FREQ_32000:
    sbc->frequency = SBC_FREQ_32000;
    break;
  case BT_SBC_SAMPLING_FREQ_44100:
    sbc->frequency = SBC_FREQ_44100;
    break;
  case BT_SBC_SAMPLING_FREQ_48000:
    sbc->frequency = SBC_FREQ_48000;
    break;
  default:
    fprintf (stderr, "No supported frequency");
  }

  switch (cap->channel_mode)
  {
  case BT_A2DP_CHANNEL_MODE_MONO:
    sbc->mode = SBC_MODE_MONO;
    break;
  case BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL:
    sbc->mode = SBC_MODE_DUAL_CHANNEL;
    break;
  case BT_A2DP_CHANNEL_MODE_STEREO:
    sbc->mode = SBC_MODE_STEREO;
    break;
  case BT_A2DP_CHANNEL_MODE_JOINT_STEREO:
    sbc->mode = SBC_MODE_JOINT_STEREO;
    break;
  default:
    fprintf (stderr, "No supported channel_mode");
  }

  switch (cap->allocation_method)
  {
  case BT_A2DP_ALLOCATION_SNR:
    sbc->allocation = SBC_AM_SNR;
    break;
  case BT_A2DP_ALLOCATION_LOUDNESS:
    sbc->allocation = SBC_AM_LOUDNESS;
    break;
  default:
    fprintf (stderr, "No supported allocation");
  }

  switch (cap->subbands)
  {
  case BT_A2DP_SUBBANDS_4:
    sbc->subbands = SBC_SB_4;
    break;
  case BT_A2DP_SUBBANDS_8:
    sbc->subbands = SBC_SB_8;
    break;
  default:
    fprintf (stderr, "No supported subbands");
  }

  switch (cap->block_length)
  {
  case BT_A2DP_BLOCK_LENGTH_4:
    sbc->blocks = SBC_BLK_4;
    break;
  case BT_A2DP_BLOCK_LENGTH_8:
    sbc->blocks = SBC_BLK_8;
    break;
  case BT_A2DP_BLOCK_LENGTH_12:
    sbc->blocks = SBC_BLK_12;
    break;
  case BT_A2DP_BLOCK_LENGTH_16:
    sbc->blocks = SBC_BLK_16;
    break;
  default:
    fprintf (stderr, "No supported block length");
  }

  sbc->bitpool = cap->max_bitpool;
}

//////////////////////////////// BLUEZ AUDIO CALLBACK HANDLER ////////////////////////////////


/*****************//**
     * Implement MediaEndpoint1.SelectConfiguration.
     * Called by bluez to negotiate which configuration (=codec, codec parameter)
     * for audio streaming.
     * This function will examine what the requested configuration and returns back
     * a reply with the supported / agreed configuration.
     *
     * Chosen configuration isn't cached because it will be returned with SetConfiguration.
     *
     * Contains modified code taken from pulseaudio 2.1 (which took it from
     * bluez audio, select_sbc_params (a2dp.c)
     *
     * @param [in] original "call" message from bluez
     * @returns reply message (success or failure)
     *********************/
DBusMessage* endpoint_select_configuration (DBusMessage *msg)
{
  a2dp_sbc_t *cap, config;
  uint8_t *pconf = (uint8_t *) &config;
  int size;
  DBusMessage *reply;
  DBusError err;

  debug_print ("Select configuration\n");
  dbus_error_init(&err);

  if (!dbus_message_get_args(msg, &err, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &cap, &size, DBUS_TYPE_INVALID)) {
    handle_dbus_error (&err, __FUNCTION__, __LINE__);
    goto fail;
  }

  //taken from pulseaudio with modification
  memset(&config, 0, sizeof(config));
  config.frequency = BT_SBC_SAMPLING_FREQ_44100;
  if (cap->channel_mode & BT_A2DP_CHANNEL_MODE_JOINT_STEREO)
    config.channel_mode = BT_A2DP_CHANNEL_MODE_JOINT_STEREO;
  else if (cap->channel_mode & BT_A2DP_CHANNEL_MODE_STEREO)
    config.channel_mode = BT_A2DP_CHANNEL_MODE_STEREO;
  else if (cap->channel_mode & BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL)
    config.channel_mode = BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL;
  else if (cap->channel_mode & BT_A2DP_CHANNEL_MODE_MONO) {
    config.channel_mode = BT_A2DP_CHANNEL_MODE_MONO;
  } else {
    fprintf (stderr, "No supported channel modes");
    goto fail;
  }

  if (cap->block_length & BT_A2DP_BLOCK_LENGTH_16)
    config.block_length = BT_A2DP_BLOCK_LENGTH_16;
  else if (cap->block_length & BT_A2DP_BLOCK_LENGTH_12)
    config.block_length = BT_A2DP_BLOCK_LENGTH_12;
  else if (cap->block_length & BT_A2DP_BLOCK_LENGTH_8)
    config.block_length = BT_A2DP_BLOCK_LENGTH_8;
  else if (cap->block_length & BT_A2DP_BLOCK_LENGTH_4)
    config.block_length = BT_A2DP_BLOCK_LENGTH_4;
  else {
    fprintf (stderr, "No supported block lengths");
    goto fail;
  }

  if (cap->subbands & BT_A2DP_SUBBANDS_8)
    config.subbands = BT_A2DP_SUBBANDS_8;
  else if (cap->subbands & BT_A2DP_SUBBANDS_4)
    config.subbands = BT_A2DP_SUBBANDS_4;
  else {
    fprintf (stderr, "No supported subbands");
    goto fail;
  }

  if (cap->allocation_method & BT_A2DP_ALLOCATION_LOUDNESS)
    config.allocation_method = BT_A2DP_ALLOCATION_LOUDNESS;
  else if (cap->allocation_method & BT_A2DP_ALLOCATION_SNR)
    config.allocation_method = BT_A2DP_ALLOCATION_SNR;

  config.min_bitpool = (uint8_t) MAX(MIN_BITPOOL, cap->min_bitpool);
  config.max_bitpool = (uint8_t) MIN(a2dp_default_bitpool(config.frequency, config.channel_mode), cap->max_bitpool);

  reply = dbus_message_new_method_return(msg);
  dbus_message_append_args (reply,
                            DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &pconf, size,
                            DBUS_TYPE_INVALID);
  return reply;

fail:
  return dbus_message_new_error(msg, "org.bluez.MediaEndpoint1.Error.InvalidArguments",
                                "Unable to select configuration");
}

/*****************//**
     * Implement MediaEndpoint1.SetConfiguration.
     * Called by bluez to confirm that this will be the configuration chosen.
     * The most important thing here is the "transport object path", which we will
     * need to get the actual file-descriptor for streaming later (see transport_acquire).
     *
     * In theory the transport_acquire could be called from here with some delays,
     * in reality it is a lot better to do it after we've received confirmation that
     * audio is "connected" (this is done by watching AudioSource.PropertyChange signal)
     *
     * This function is too complicated for what it does, for our (simple) purpose
     * we actually only need the transport_path so we don't have to parse all the parameters,
     * but we do anyway.
     *
     * @param [in] original "call" message from bluez
     * @param [in] io thread hashtable
     * @returns reply message (success or failure)
     *********************/
DBusMessage* endpoint_set_configuration (DBusMessage *msg, io_thread_tcb_s **io_threads_table) {
  const char *transport_path, *dev_path = NULL, *uuid = NULL, *cmd_path = NULL;
  uint8_t *config = NULL;
  int size = 0;
  DBusMessageIter iter, iterprop, iterentry, itervalue, iterarray;
  io_thread_tcb_s *head = *io_threads_table;
  io_thread_tcb_s *io_data = NULL;

  dbus_message_iter_init(msg, &iter);
  dbus_message_iter_get_basic(&iter, &transport_path);
  if (!dbus_message_iter_next(&iter))
    goto fail;

  dbus_message_iter_recurse(&iter, &iterprop);
  if (dbus_message_iter_get_arg_type(&iterprop) != DBUS_TYPE_DICT_ENTRY)
    goto fail;

  /* Read transport properties */
  while (dbus_message_iter_get_arg_type(&iterprop) == DBUS_TYPE_DICT_ENTRY) {
    const char *key;
    int var;

    dbus_message_iter_recurse(&iterprop, &iterentry);
    dbus_message_iter_get_basic(&iterentry, &key);

    dbus_message_iter_next(&iterentry);
    dbus_message_iter_recurse(&iterentry, &itervalue);

    var = dbus_message_iter_get_arg_type(&itervalue);
    if (strcasecmp(key, "UUID") == 0) {
      if (var != DBUS_TYPE_STRING)
        goto fail;
      dbus_message_iter_get_basic(&itervalue, &uuid);
    } else if (strcasecmp(key, "Device") == 0) {
      if (var != DBUS_TYPE_OBJECT_PATH)
        goto fail;
      dbus_message_iter_get_basic(&itervalue, &dev_path);
    } else if (strcasecmp(key, "Configuration") == 0) {
      if (var != DBUS_TYPE_ARRAY)
        goto fail;
      dbus_message_iter_recurse(&itervalue, &iterarray);
      dbus_message_iter_get_fixed_array(&iterarray, &config, &size);
    }
    dbus_message_iter_next(&iterprop);
  }

  char test[100];
  strcpy(test,dev_path);
  strcat(test,"/player0");

  //capture the transport_path and allocate the transport later, when the audiosource is "connected".
  HASH_FIND_STR (head, transport_path, io_data);
  if (!io_data)
  {
    io_data = create_io_thread();
    io_data->dev_path = strdup (test);
    io_data->transport_path = strdup (transport_path);
    io_data->cap = *((a2dp_sbc_t*) config);

    //read or write
    cmd_path = (char *)dbus_message_get_path (msg);
    if ( strcasecmp (cmd_path, A2DP_SINK_ENDPOINT) == 0)
    {
      io_data->write = 0;
    }
    else if ( strcasecmp (cmd_path, A2DP_SINK_ENDPOINT_SECOND) == 0)
    {
      io_data->write = 0;
    }
    else if ( strcasecmp (cmd_path, A2DP_SOURCE_ENDPOINT) == 0)
    {
      io_data->dev_path = strdup (dev_path);
      io_data->write = 1;
    }

    HASH_ADD_KEYPTR (hh, head, io_data->transport_path, strlen(io_data->transport_path), io_data);
    *io_threads_table = head;
  }

  debug_print ("Set configuration (%s) - dev_path (%s) : write (%d)\n", transport_path, io_data->dev_path, io_data->write);

  return dbus_message_new_method_return(msg);

fail:
  return dbus_message_new_error(msg, "org.bluez.MediaEndpoint1.Error.InvalidArguments",
                                "Unable to select configuration");
}

/*****************//**
     * Implement MediaEndpoint1.ClearConfiguration.
     * Called by bluez to let us know that the audio streaming process has been reset
     * for whatever reason, and we should do our own clean-up.
     * Here we tell our I/O thread to stop.
     *
     * It is not necessary to call transport_release here because by the time we got here,
     * the 'transport' has already been released.
     *
     * @param [in] original "call" message from bluez
     * @param [in] io thread's data - to command I/O thread to stop.
     * @returns reply message (success or failure)
     *********************/
DBusMessage* endpoint_clear_configuration (DBusMessage *msg, io_thread_tcb_s **io_threads_table, DBusConnection *conn) {
  DBusMessage *reply;
  DBusError err;
  DBusMessageIter iter;
  char *transport_path;
  io_thread_tcb_s *head = *io_threads_table;
  io_thread_tcb_s *io_data = NULL;

  dbus_error_init(&err);
  dbus_message_iter_init(msg, &iter);
  dbus_message_iter_get_basic(&iter, &transport_path);
  debug_print ("Clear configuration %s\n",transport_path);

  // stop stream
  HASH_FIND_STR (head, transport_path, io_data);
  if (io_data)
  {
    threadId--;
    debug_print ("stopping thread %p\n",io_data);
    HASH_DEL (head, io_data);
    *io_threads_table = head;
    destroy_io_thread (io_data);
  }

  //transport_release(conn, io_data->transport_path, io_data);

  reply = dbus_message_new_method_return(msg);
  return reply;
}

/*****************//**
     * Implement MediaEndpoint1.Release
     * Called by bluez to let us know our registration (see register_endpoint) has been
     * cancelled (or 'released'). The next logical action after this, is either:
     * a) to exit
     * b) to re-register.
     *
     * There is no need to 'Unregister' because by the time we get here, our endpoint
     * has already been de-registered.
     *
     * @param [in] original "call" message from bluez
     * @returns reply message (success or failure)
     *********************/
DBusMessage* endpoint_release (DBusMessage *msg) {
  debug_print ("Release endpoint\n");
  DBusMessage *reply;
  DBusError err;

  dbus_error_init(&err);
  reply = dbus_message_new_method_return(msg);
  return reply;
}



//////////////////////////////// BLUEZ-DBUS SIGNAL HANDLERS ////////////////////////////////

/*****************//**
     * Handle AudioSource.PropertyChanged
     * Signalled by bluez to let us know that the state of the audio source has changed.
     * We use this signal as a trigger for 'delayed' transport_acquire to get the file
     * descriptor, as well as to start the I/O thread.
     *
     * We don't use the corresponding transport_release because for the time being
     * we will never willingly release a transport, until it is closed by
     * MediaEndpoint1.ClearConfiguration (the I/O thread will be stopped there).
     *
     * Note: The 'source' and 'sink' terms used in bluez is super-confusing because
     * they are not consistent - sometimes they view it from the bluez side
     * (in this case, it's a 'source' because the bluez is the 'source' of the data,
     * sometimes they view it as 'sink' because this application receives and acts as data sink
     * for the remote-end.
     *
     * Note: There is a corresponding signal for AudioSink, which we don't use.
     *
     * @param [in] connection object to talk to DBus
     * @param [in] original "call" message from bluez
     * @param [in] write==0 -> audiosink, write==1 --> audiosource
     * @param [in] head of I/O thread hashtable
     * @returns reply message (success or failure)
     *********************/

#define audiosink_property_changed audiosource_property_changed
void callAdded_property_changed (DBusConnection *conn, DBusMessage *msg, int write, io_thread_tcb_s **io_threads_table) {
  DBusMessageIter iter;
  char *key, *statusString= "stopped";
  char *dev_path;
  io_thread_tcb_s *head = *io_threads_table;
  io_thread_tcb_s *io_data;
  int transition, when_to_acquire, when_to_release;

  dbus_message_iter_init(msg, &iter);

  dbus_message_iter_get_basic(&iter, &key);
  debug_print ("[NEW STATUS (Ofono) - CallAdded ...] for (%s)\n", key);

  dev_path = (char *)dbus_message_get_path (msg);
  statusString =
      debug_print ("state for %s: %s\n", dev_path, statusString);

  io_data = head;

  debug_print ("[STEP] - START - READ :\n");
  if (ofono_transport_acquire (conn, io_data->transport_path, &io_data->fd, &io_data->read_mtu, &io_data->write_mtu))
  {
    debug_print ("[DEBUG] - Audio streaming START - READ :\n");
    debug_print ("fd: %d read mtu %d write mtu %d\n", io_data->fd, io_data->read_mtu, io_data->write_mtu);
    io_thread_set_command (io_data, IO_CMD_RUNNING);
  }
  return;

fail:
  debug_print ("bad signal\n");
}



//////////////////////////////// BLUEZ-DBUS SIGNAL HANDLERS ////////////////////////////////

/*****************//**
     * Handle AudioSource.PropertyChanged
     * Signalled by bluez to let us know that the state of the audio source has changed.
     * We use this signal as a trigger for 'delayed' transport_acquire to get the file
     * descriptor, as well as to start the I/O thread.
     *
     * We don't use the corresponding transport_release because for the time being
     * we will never willingly release a transport, until it is closed by
     * MediaEndpoint1.ClearConfiguration (the I/O thread will be stopped there).
     *
     * Note: The 'source' and 'sink' terms used in bluez is super-confusing because
     * they are not consistent - sometimes they view it from the bluez side
     * (in this case, it's a 'source' because the bluez is the 'source' of the data,
     * sometimes they view it as 'sink' because this application receives and acts as data sink
     * for the remote-end.
     *
     * Note: There is a corresponding signal for AudioSink, which we don't use.
     *
     * @param [in] connection object to talk to DBus
     * @param [in] original "call" message from bluez
     * @param [in] write==0 -> audiosink, write==1 --> audiosource
     * @param [in] head of I/O thread hashtable
     * @returns reply message (success or failure)
     *********************/
#define audiosink_property_changed audiosource_property_changed
void callRemoved_property_changed (DBusConnection *conn, DBusMessage *msg, int write, io_thread_tcb_s **io_threads_table) {
  DBusMessageIter iter;
  char *key, *statusString= "stopped";
  char *dev_path;
  io_thread_tcb_s *head = *io_threads_table;
  io_thread_tcb_s *io_data;
  int transition, when_to_acquire, when_to_release;

  dbus_message_iter_init(msg, &iter);

  dbus_message_iter_get_basic(&iter, &key);

  dev_path = (char *)dbus_message_get_path (msg);
  debug_print ("state for %s: %s\n", dev_path, statusString);

  if (!head) return;
  io_data = head;
  do {
    if (strcasecmp (dev_path, io_data->dev_path) == 0 && io_data->write == write)
    {
      break;
    }
    else
    {
      //io_data = io_data->hh.next;
    }
  } while (io_data && io_data != head);

  debug_print ("[DEBUG] - Audio streaming STOP\n");
  transport_release (conn, io_data->transport_path, io_data);
  io_thread_set_command (io_data, IO_CMD_IDLE);
  return;

fail:
  debug_print ("bad signal\n");
}




//////////////////////////////// BLUEZ-DBUS SIGNAL HANDLERS ////////////////////////////////

/*****************//**
     * Handle AudioSource.PropertyChanged
     * Signalled by bluez to let us know that the state of the audio source has changed.
     * We use this signal as a trigger for 'delayed' transport_acquire to get the file
     * descriptor, as well as to start the I/O thread.
     *
     * We don't use the corresponding transport_release because for the time being
     * we will never willingly release a transport, until it is closed by
     * MediaEndpoint1.ClearConfiguration (the I/O thread will be stopped there).
     *
     * Note: The 'source' and 'sink' terms used in bluez is super-confusing because
     * they are not consistent - sometimes they view it from the bluez side
     * (in this case, it's a 'source' because the bluez is the 'source' of the data,
     * sometimes they view it as 'sink' because this application receives and acts as data sink
     * for the remote-end.
     *
     * Note: There is a corresponding signal for AudioSink, which we don't use.
     *
     * @param [in] connection object to talk to DBus
     * @param [in] original "call" message from bluez
     * @param [in] write==0 -> audiosink, write==1 --> audiosource
     * @param [in] head of I/O thread hashtable
     * @returns reply message (success or failure)
     *********************/
#define audiosink_property_changed audiosource_property_changed
void audiosource_property_changed (DBusConnection *conn, DBusMessage *msg, int write, io_thread_tcb_s **io_threads_table) {
  DBusMessageIter iter, itervariant, iterState, subiter;
  char *key, *statusString= "stopped", *keyStatus;
  char *dev_path;
  io_thread_tcb_s *head = *io_threads_table;
  io_thread_tcb_s *io_data;
  int new_state, transition, when_to_acquire, when_to_release;

  dbus_bool_t valBool;
  dbus_message_iter_init(msg, &iter);
  dbus_message_iter_get_basic(&iter, &key);

  char *devType = "noType";
  statusString="noStatus";

  if (strcasecmp(key, "org.bluez.MediaControl1") == 0)
  {
    while (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_INVALID)
    {
      dbus_message_iter_next (&iter);

      if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY)
      {
        dbus_message_iter_recurse(&iter, &itervariant);

        if (dbus_message_iter_get_arg_type(&itervariant) == DBUS_TYPE_DICT_ENTRY)
        {
          dbus_message_iter_recurse(&itervariant, &iterState);

          while (dbus_message_iter_get_arg_type (&iterState) != DBUS_TYPE_INVALID)
          {
            if (dbus_message_iter_get_arg_type(&iterState) == DBUS_TYPE_STRING)
            {
              dbus_message_iter_get_basic(&iterState, &keyStatus);
            }
            else if (dbus_message_iter_get_arg_type(&iterState) == DBUS_TYPE_VARIANT)
            {
              dbus_message_iter_recurse (&iterState, &subiter);
              while (dbus_message_iter_get_arg_type (&subiter) != DBUS_TYPE_INVALID)
              {
                if (dbus_message_iter_get_arg_type(&subiter) == DBUS_TYPE_BOOLEAN)
                {
                  dbus_message_iter_get_basic (&subiter, &valBool);
                  if(valBool)
                  {
                    devType = "headset";
                    statusString="playing";
                  }
                  else
                  {
                    devType = "headset";
                    statusString="paused";
                  }
                }
                dbus_message_iter_next (&subiter);
              }
            }
            dbus_message_iter_next (&iterState);
          }
        }
      }
    }
  }
  else if (strcasecmp(key, "org.bluez.MediaPlayer1") == 0)
  {
    while (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_INVALID)
    {
      dbus_message_iter_next (&iter);

      if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY)
      {
        dbus_message_iter_recurse(&iter, &itervariant);

        if (dbus_message_iter_get_arg_type(&itervariant) == DBUS_TYPE_DICT_ENTRY)
        {
          dbus_message_iter_recurse(&itervariant, &iterState);

          while (dbus_message_iter_get_arg_type (&iterState) != DBUS_TYPE_INVALID)
          {
            if (dbus_message_iter_get_arg_type(&iterState) == DBUS_TYPE_STRING)
            {
              dbus_message_iter_get_basic(&iterState, &keyStatus);
            }
            else if (dbus_message_iter_get_arg_type(&iterState) == DBUS_TYPE_VARIANT)
            {
              dbus_message_iter_recurse (&iterState, &subiter);
              while (dbus_message_iter_get_arg_type (&subiter) != DBUS_TYPE_INVALID)
              {
                if (dbus_message_iter_get_arg_type(&subiter) == DBUS_TYPE_STRING)
                {
                  if (strcasecmp(keyStatus, "Status") != 0)
                  {
                    devType = "a2dp";
                    devNumber++;
                    goto fail;
                  }
                  else
                  {
                    devType = "a2dp";
                    devNumber++;
                    dbus_message_iter_get_basic(&subiter, &statusString);
                  }
                }
                else
                {
                  goto fail;
                }
                dbus_message_iter_next (&subiter);
              }
            }
            dbus_message_iter_next (&iterState);
          }
        }
      }
    }
  }
  else
  {
    goto fail;
  }

  dev_path = (char *)dbus_message_get_path (msg);
  debug_print ("state for %s: %s (type : (%s))\n", dev_path, statusString, devType);

  if (!head) return;
  io_data = head;
  do
  {
    if (strcasecmp (dev_path, io_data->dev_path) == 0)
    {
      break;
    }
    else
    {
      io_data = io_data->hh.next;
    }
  }while (io_data && io_data != head);

  if (!io_data) return;

  //decode state & transition
  new_state = transition = -1;
  if ( strcasecmp (statusString,      "paused")  == 0 ) new_state = STATE_CONNECTED;
  else if ( strcasecmp (statusString, "playing") == 0 ) new_state = STATE_PLAYING;
  else if ( strcasecmp (statusString, "stopped") == 0 ) new_state = STATE_DISCONNECTED;  // stopped - paused

  if (new_state >= 0)
  {
    transition = io_data->prev_state << 4 | new_state;
    io_data->prev_state = new_state;
  }

  //our treatment of sink and source is a bit different
  if(io_data->write == 0)
  {
    // bt sink: bt --> alsa
    debug_print ("AUDIO SOURCE : bt --> alsa\n");
    when_to_acquire = STATE_CONNECTED << 4 | STATE_PLAYING;
    when_to_release = STATE_PLAYING << 4 | STATE_CONNECTED;
  }
  else if(io_data->write == 1)
  {
    // bt source: alsa --> bt
    debug_print ("AUDIO SINK : alsa --> bt\n");
    when_to_acquire = STATE_DISCONNECTED << 4 | STATE_CONNECTED;
    when_to_release = STATE_CONNECTED << 4 | STATE_DISCONNECTED;
  }

  io_data->connData = conn;

  //acquire or release transport depending on the transitions
  if (strcasecmp (statusString, "playing") == 0)
  {
    if(io_data->streamStatus == 1)
    {
      if(io_data->write == 0)
      {
        debug_print(">>>>>>PHONE>>>>\n");
        io_thread_set_command (io_data, IO_CMD_RUNNING);
      }
    }
    else
    {
      if (transport_acquire(conn, io_data->transport_path, &io_data->fd, &io_data->read_mtu, &io_data->write_mtu))
      {
        debug_print ("[DEBUG]-Audio streaming START = fd: %d read mtu %d write mtu %d\n", io_data->fd, io_data->read_mtu, io_data->write_mtu);
        if(io_data->write == 0)
        {
          debug_print(">>>>>>PHONE>>>>\n");
          io_thread_set_command (io_data, IO_CMD_RUNNING);
        }
        else if(io_data->write == 1)
        {
          debug_print(">>>>>>HEADSET>>>>\n");
          io_thread_set_command (io_data, IO_CMD_RUN_HEADSET);
        }
      }
      else
      {
        debug_print("[DEBUG] - ERROR at transport_acquire\n");
      }
    }
    statusString = "stopped";
    new_state = transition = -1;
  }
  else
  {
    debug_print ("\n[DEBUG] - Audio streaming STOP\n");
    if(io_data->streamStatus == 2)
    {
      transport_release (conn, io_data->transport_path, io_data);
    }
    io_thread_set_command (io_data, IO_CMD_IDLE);
  }
  return;

fail:
  debug_print ("...\n");
}



//////////////////////////////// IO THREAD HELPERS ////////////////////////////////


/*****************//**
     * Send command to I/O thread in a thread-safe manner.
     * Once given, it will also trigger I/O thread to start running (if it is not
     * already is).
     *
     * Note: Once IO_CMD_TERMINATE is issued, it cannot be cancelled.
     *
     * @param [in] I/O thread control block
     * @param [in] command to send
     * @returns reply message (success or failure)
     *********************/
void io_thread_set_command (io_thread_tcb_s *data, int command) {
  //pthread_mutex_lock (&data->mutex);
  debug_print ("io cmd: %d\n", command);
  if (data->command != IO_CMD_TERMINATE)
    data->command = command;
  //pthread_mutex_unlock (&data->mutex);
  pthread_cond_signal (&data->cond);
}


/*****************//**
     * Create an I/O thread.
     * This will create the thread control block and the thread itself.
     * The created thread is already running but in suspended state.
     *
     * @returns newly created thread control block
     *********************/
void *io_thread_run(void *ptr);
void *stream_bt_input(void *ptr);
io_thread_tcb_s *create_io_thread()
{
  io_thread_tcb_s *p;

  p = malloc (sizeof (io_thread_tcb_s));
  memset (p, 0, sizeof (io_thread_tcb_s));
  pthread_cond_init (&p->cond, NULL);
  pthread_mutex_init (&p->mutex, NULL);
  threadId++;
  p->devId = threadId;
  p->streamStatus = 0;
  sbc_init (&p->sbc, 0);
  debug_print ("\nThread ID : %d\n", threadId);
  pthread_create (&p->t_handle, NULL, io_thread_run, p);
  return p;
}

/*****************//**
     * Destroy and existing an I/O thread.
     * This will stop the terminate the thread, wait until it is really terminated,
     * release the resources held by the thread, then free the tcb itself.
     *
     * @param [in] thread control block
     *********************/
void destroy_io_thread(io_thread_tcb_s *p) {
  if (p) {
    io_thread_set_command (p, IO_CMD_TERMINATE);
    pthread_join (p->t_handle, NULL);
    pthread_cond_destroy (&p->cond);
    pthread_mutex_destroy (&p->mutex);
    if (p->transport_path) {
      free (p->transport_path);
      p->transport_path = NULL;
    }
    if (p->dev_path) {
      free (p->dev_path);
      p->dev_path = NULL;
    }
    free (p);
  }
}


static int setup_handle(snd_pcm_t *handle, unsigned int rate, snd_pcm_uframes_t *psize)
{
  int err;
  snd_pcm_hw_params_t *hw_params;

  if((err = snd_pcm_hw_params_malloc(&hw_params)) < 0)
  {
    fprintf(stderr, "cannot allocate hardware parameter structure (%s)\n", snd_strerror(err));
    return 0;
  }

  if((err = snd_pcm_hw_params_any(handle, hw_params)) < 0)
  {
    fprintf(stderr, "cannot initialize hardware parameter structure (%s)\n", snd_strerror(err));
    return 0;
  }

  if((err = snd_pcm_hw_params_set_access(handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
  {
    fprintf(stderr, "cannot set access type (%s)\n", snd_strerror(err));
    return 0;
  }

  if((err = snd_pcm_hw_params_set_format(handle, hw_params, SND_PCM_FORMAT_S16_LE)) < 0)
  {
    fprintf(stderr, "cannot set sample format (%s)\n", snd_strerror(err));
    return 0;
  }

  if((err = snd_pcm_hw_params_set_rate_near(handle, hw_params, &rate, 0)) < 0)
  {
    fprintf(stderr, "cannot set sample rate (%s)\n", snd_strerror(err));
    return 0;
  }

  if((err = snd_pcm_hw_params_set_channels(handle, hw_params, 2)) < 0)
  {
    fprintf(stderr, "cannot set channel count (%s)\n", snd_strerror(err));
    return 0;
  }

  if((err = snd_pcm_hw_params(handle, hw_params)) < 0)
  {
    fprintf(stderr, "cannot set parameters (%s)\n", snd_strerror(err));
    return 0;
  }

  snd_pcm_hw_params_get_period_size(hw_params, psize, 0);
  snd_pcm_hw_params_free(hw_params);

  return 1;
}


//////////////////////////////// IO THREAD ////////////////////////////////

/*****************//**
     * Read stdin, encode it, and write output to bluez stream output
     * Encoding function is taken from pulseaudio 2.1
     *
     * @param [in] I/O thread control block
     * @param [in] run_once global variable
     * @param [out] quit global variable (if the app should quit)
     *********************/
void *stream_bt_output(void *ptr)
{
  debug_print ("write to bt -> Headset\n");
  io_thread_tcb_s *data = ptr;
  void *buf, *encode_buf;
  size_t bufsize, encode_bufsize;
  struct pollfd  pollout = { data->fd, POLLOUT, 0 };
  int timeout;

  // get buffers
  encode_bufsize = data->write_mtu;
  encode_buf = malloc (encode_bufsize);
  bufsize = (encode_bufsize / sbc_get_frame_length (&data->sbc)) * // max frames allowed in a packet
      sbc_get_codesize(&data->sbc); // ensure all of our source will fit in a single packet
  buf = malloc (bufsize);
  debug_print ("encode_buf %d buf %d", encode_bufsize, bufsize);

  /// =================================== READ FROM PCM CAPTURE ================================
  char *capture_device = "A2DP_capture_0";
  snd_pcm_t *capture_handle;
  snd_pcm_uframes_t capture_psize;
  unsigned int rate = 44100;
  int err;

  if((err = snd_pcm_open(&capture_handle, capture_device, SND_PCM_STREAM_CAPTURE, 0)) < 0)
  {
    debug_print ("cannot open audio device %s (%s)\n", capture_device, snd_strerror(err));
    return 1;
  }

  if(!setup_handle(capture_handle, rate, &capture_psize))
    return 1;
  if((err = snd_pcm_prepare(capture_handle)) < 0)
  {
    debug_print ("cannot prepare audio interface for use (%s)\n", snd_strerror(err));
    return 1;
  }
  /// ===========================================================================================

  // stream
  while (data->command == IO_CMD_RUNNING)
  {
    ssize_t readlen;
    /// READ FROM PCM CAPTURE ...................................................

    debug_print("[DEBUG] step 1 \n");

    int persize = bufsize;
    snd_pcm_sframes_t received_frames = 0;
    int received = 0;
    do{
      received_frames = snd_pcm_readi(capture_handle,
                                      (char *)buf + received,
                                      snd_pcm_bytes_to_frames(capture_handle,persize - received));
      if(received_frames < 0)
      {
        break;
      }
      debug_print("READ = rec %d  size %d \n", received, persize);
      received += snd_pcm_frames_to_bytes(capture_handle, received_frames);
    } while (received < persize);

    /// .........................................................................

    struct rtp_header *header;
    struct rtp_payload *payload;
    size_t nbytes;

    header = encode_buf;
    payload = (struct rtp_payload*) ((uint8_t*) encode_buf + sizeof(*header));

    void *p = buf;
    void *d = encode_buf + sizeof(*header) + sizeof(*payload);
    size_t to_write = encode_bufsize - sizeof(*header) - sizeof(*payload);
    size_t to_encode = readlen;
    unsigned frame_count = 0;

    while (to_encode >= sbc_get_codesize(&data->sbc))
    {
      //debug_print ("%zu ", to_encode);
      ssize_t written;
      ssize_t encoded;

      //debug_print ("%p %d %d\n", d, to_write, sbc_get_frame_length (&data->sbc));
      encoded = sbc_encode(&data->sbc,
                           p, to_encode,
                           d, to_write,
                           &written);

      if (encoded <= 0)
      {
        data->streamStatus == 0;
        pthread_exit(0);
        //debug_print ("SBC encoding error %zd\n", encoded);
        break; // make do with what have
      }

      p = (uint8_t*) p + encoded;
      to_encode -= encoded;
      d = (uint8_t*) d + written;
      to_write -= written;

      frame_count++;
    }

    // encapsulate it in a2dp RTP packets
    memset(encode_buf, 0, sizeof(*header) + sizeof(*payload));
    header->v = 2;
    header->pt = 1;
    header->sequence_number = htons(data->seq_num++);
    header->timestamp = htonl(data->timestamp);
    header->ssrc = htonl(1);
    payload->frame_count = frame_count;

    // next timestamp
    data->timestamp += sbc_get_frame_duration(&data->sbc) * frame_count;

    // how much to output
    nbytes = (uint8_t*) d - (uint8_t*) encode_buf;

    //debug_print ("nbytes: %zu\n", nbytes);
    if (!nbytes) break; // don't write if there is nothing to write

    // wait until bluetooth is ready
    while (data->command == IO_CMD_RUNNING)
    {
      //debug_print ("waiting for bluetooth\n");
      pthread_mutex_unlock (&data->mutex);
      timeout = poll (&pollout, 1, 1000); //delay 1s to allow others to update our state
      pthread_mutex_lock (&data->mutex);
      if (timeout == 0) continue;
      if (timeout < 0) fprintf (stderr, "bt_write/bluetooth: %d\n", errno);
      break;
    }

    // write bluetooth
    if (timeout > 0)
    {
      debug_print ("flush bluetooth\n");
      write (data->fd, encode_buf, nbytes);
    }
  }

  // cleanup
  snd_pcm_drain(capture_handle);
  snd_pcm_close(capture_handle);
  free (buf);
  free (encode_buf);
  debug_print("FINISH\n");
  data->streamStatus == 0;
  pthread_exit(0);
}

void stream_bt_outputTEST(io_thread_tcb_s *data)
{
  void *bufHEADSET, *encode_buf_HEADSET;
  size_t bufsize_HEADSET, encode_bufsize_HEADSET;
  struct pollfd pollout = { data->fd, POLLOUT, 0 };
  int timeout_HEADSET;

  debug_print ("write to bt (to fd - %d)\n", data->fd);

  // get buffers
  encode_bufsize_HEADSET = data->write_mtu;
  encode_buf_HEADSET = malloc (encode_bufsize_HEADSET);
  bufsize_HEADSET = (encode_bufsize_HEADSET / sbc_get_frame_length (&data->sbc)) * // max frames allowed in a packet
  sbc_get_codesize(&data->sbc); // ensure all of our source will fit in a single packet
  bufHEADSET = malloc (bufsize_HEADSET);
  debug_print ("encode_buf %d buf %d", encode_bufsize_HEADSET, bufsize_HEADSET);

  /// =================================== READ FROM PCM CAPTURE ================================
  char *capture_device = "A2DP_capture_0";
  snd_pcm_t *capture_handle;
  snd_pcm_uframes_t capture_psize;
  unsigned int rate_HEADSET = 44100;
  int err;

   if((err = snd_pcm_open(&capture_handle, capture_device, SND_PCM_STREAM_CAPTURE, 0)) < 0)
   {
     debug_print ("cannot open audio device %s (%s)\n", capture_device, snd_strerror(err));
     return 1;
   }

   if(!setup_handle(capture_handle, rate_HEADSET, &capture_psize))
     return 1;
   //cbuf = (short *)malloc(capture_psize * 2);
   //printf("Recording 5-second audio clip to play.raw\n");
   if((err = snd_pcm_prepare(capture_handle)) < 0)
   {
     debug_print ("cannot prepare audio interface for use (%s)\n", snd_strerror(err));
     return 1;
}

   /// ===========================================================================================

  // stream
  while (data->command == IO_CMD_RUN_HEADSET)
  {
    ssize_t readlen;

    /// READ FROM PCM CAPTURE ...................................................

    int persize = bufsize_HEADSET;
    snd_pcm_sframes_t received_frames_HEADSET = 0;
    int received = 0;
    do{
        received_frames_HEADSET = snd_pcm_readi(capture_handle,
                                     (char *)bufHEADSET + received,
                                     snd_pcm_bytes_to_frames(capture_handle,persize - received));
        if(received_frames_HEADSET < 0)
        {
            break;
        }
        //debug_print("READ = rec %d  size %d \n", received, persize);
        received += snd_pcm_frames_to_bytes(capture_handle, received_frames_HEADSET);
    } while (received < persize);

    /// .........................................................................

    struct rtp_header *header;
    struct rtp_payload *payload;
    size_t nbytes;

    header = encode_buf_HEADSET;
    payload = (struct rtp_payload*) ((uint8_t*) encode_buf_HEADSET + sizeof(*header));

    void *p = bufHEADSET;
    void *d = encode_buf_HEADSET + sizeof(*header) + sizeof(*payload);
    size_t to_write = encode_bufsize_HEADSET - sizeof(*header) - sizeof(*payload);
    size_t to_encode = readlen;
    unsigned frame_count = 0;

    while (to_encode >= sbc_get_codesize(&data->sbc))
    {
      //debug_print ("%zu ", to_encode);
      ssize_t written;
      ssize_t encoded;

      //debug_print ("%p %d %d\n", d, to_write, sbc_get_frame_length (&data->sbc));
      encoded = sbc_encode(&data->sbc,
                           p, to_encode,
                           d, to_write,
                           &written);

      if (encoded <= 0) {
        //debug_print ("SBC encoding error %zd\n", encoded);
        break; // make do with what have
      }

      p = (uint8_t*) p + encoded;
      to_encode -= encoded;
      d = (uint8_t*) d + written;
      to_write -= written;

      frame_count++;
    }

    // encapsulate it in a2dp RTP packets
    memset(encode_buf_HEADSET, 0, sizeof(*header) + sizeof(*payload));
    header->v = 2;
    header->pt = 1;
    header->sequence_number = htons(data->seq_num++);
    header->timestamp = htonl(data->timestamp);
    header->ssrc = htonl(1);
    payload->frame_count = frame_count;

    // next timestamp
    data->timestamp += sbc_get_frame_duration(&data->sbc) * frame_count;

    // how much to output
    nbytes = (uint8_t*) d - (uint8_t*) encode_buf_HEADSET;

    //debug_print ("nbytes: %zu\n", nbytes);
    if (!nbytes) break; // don't write if there is nothing to write

    // wait until bluetooth is ready
    while (data->command == IO_CMD_RUN_HEADSET) {
      //debug_print ("waiting for bluetooth\n");
      timeout_HEADSET = poll (&pollout, 1, 1000); //delay 1s to allow others to update our state
      if (timeout_HEADSET == 0) continue;
      if (timeout_HEADSET < 0) fprintf (stderr, "bt_write/bluetooth: %d\n", errno);
      break;
    }

    // write bluetooth
    if (timeout_HEADSET > 0)
    {
      //debug_print ("flush bluetooth\n");
      write (data->fd, encode_buf_HEADSET, nbytes);
    }
  }

  // cleanup
  snd_pcm_close(capture_handle);
  free (bufHEADSET);
  free (encode_buf_HEADSET);
}


static uint64_t timespec_us(const struct timespec *ts) {
  return
      ts->tv_sec * 1000000LLU +
      ts->tv_nsec / 1000LLU;
}

void *pa_push(void *ptr)
{
  io_thread_tcb_s *data = ptr;
  int r;
  snd_pcm_hw_params_t *hwparams;
  snd_pcm_sw_params_t *swparams;
  snd_pcm_status_t *status;
  unsigned rate = 44100;
  unsigned periods = 2;
  snd_pcm_uframes_t boundary, buffer_size = 44100 / 10; /* 100s - 44100/10 */
  int dir = 1;
  struct pollfd *pollfds;
  int n_pollfd;

  //snd_pcm_t *data->pcm;

  /// ********************************************

  int timeout;
  size_t bufsize, decode_bufsize;
  void *buf, *decode_buf;

  /// ********************************************

  snd_pcm_hw_params_alloca(&hwparams);
  snd_pcm_sw_params_alloca(&swparams);
  snd_pcm_status_alloca(&status);

  if(data->devId == 1)
  {
    r = snd_pcm_open(&data->pcm, PCM_DEVICE, SND_PCM_STREAM_PLAYBACK, 0);
    assert(r == 0);
  }
  else
  {
    r = snd_pcm_open(&data->pcm, PCM_DEVICE_SECOND, SND_PCM_STREAM_PLAYBACK, 0);
    assert(r == 0);
  }

  r = snd_pcm_hw_params_any(data->pcm, hwparams);
  assert(r == 0);
  r = snd_pcm_hw_params_set_rate_resample(data->pcm, hwparams, 1);
  assert(r == 0);
  r = snd_pcm_hw_params_set_access(data->pcm, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED);
  assert(r == 0);
  r = snd_pcm_hw_params_set_format(data->pcm, hwparams, SND_PCM_FORMAT_S16_LE);
  assert(r == 0);
  r = snd_pcm_hw_params_set_rate_near(data->pcm, hwparams, &rate, NULL);
  assert(r == 0);
  r = snd_pcm_hw_params_set_channels(data->pcm, hwparams, 2);
  assert(r == 0);
  r = snd_pcm_hw_params_set_periods_integer(data->pcm, hwparams);
  assert(r == 0);
  r = snd_pcm_hw_params_set_periods_near(data->pcm, hwparams, &periods, &dir);
  assert(r == 0);
  r = snd_pcm_hw_params_set_buffer_size_near(data->pcm, hwparams, &buffer_size);
  assert(r == 0);
  r = snd_pcm_hw_params(data->pcm, hwparams);
  assert(r == 0);
  r = snd_pcm_hw_params_current(data->pcm, hwparams);
  assert(r == 0);
  r = snd_pcm_sw_params_current(data->pcm, swparams);
  assert(r == 0);
  r = snd_pcm_sw_params_set_avail_min(data->pcm, swparams, 1);
  assert(r == 0);
  r = snd_pcm_sw_params_set_period_event(data->pcm, swparams, 0);
  assert(r == 0);
  r = snd_pcm_hw_params_get_buffer_size(hwparams, &buffer_size);
  assert(r == 0);
  r = snd_pcm_sw_params_set_start_threshold(data->pcm, swparams, buffer_size);
  assert(r == 0);
  r = snd_pcm_sw_params_get_boundary(swparams, &boundary);
  assert(r == 0);
  r = snd_pcm_sw_params_set_stop_threshold(data->pcm, swparams, boundary);
  assert(r == 0);
  r = snd_pcm_sw_params_set_tstamp_mode(data->pcm, swparams, SND_PCM_TSTAMP_ENABLE);
  assert(r == 0);
  r = snd_pcm_sw_params(data->pcm, swparams);
  assert(r == 0);
  r = snd_pcm_prepare(data->pcm);
  assert(r == 0);
  r = snd_pcm_sw_params_current(data->pcm, swparams);
  assert(r == 0);
  n_pollfd = snd_pcm_poll_descriptors_count(data->pcm);
  assert(n_pollfd > 0);
  pollfds = malloc(sizeof(struct pollfd) * n_pollfd);
  assert(pollfds);
  r = snd_pcm_poll_descriptors(data->pcm, pollfds, n_pollfd);
  assert(r == n_pollfd);

  printf("Starting. Buffer size is %u frames\n", (unsigned int) buffer_size);
  struct pollfd pollin = { data->fd, POLLIN, 0 };

  snd_pcm_sframes_t avail, delay;
  struct timespec timestamp;
  unsigned short revents;
  snd_pcm_state_t state;
  ssize_t readlen=0;
  struct rtp_header *header;
  struct rtp_payload *payload;

  size_t written;
  size_t decoded;

  snd_pcm_sframes_t sent_frames = 0;
  int sent = 0;

  for(;;)
  {
    timeout = poll(&pollin, 1, 1000); //delay 1s to allow others to update our state
    debug_print("......................................(%d)\n",timeout);
    if (timeout == 0)
    {
      continue;
    }
    else if (timeout < 0)
    {
      data->command = IO_CMD_IDLE;
      //break;
    }


  while(POLLIN == (pollin.revents & POLLIN) && timeout > 0)
  {
    debug_print(".......POLL in .....\n");


    typeof(data->read_mtu) _a = (data->read_mtu);                 \
    typeof(data->write_mtu) _b = (data->write_mtu);                 \

    size_t min_buffer_size = _a > _b ? _a : _b;

    bufsize = 2 * min_buffer_size;
    free(buf);
    free(decode_buf);
    buf = malloc(bufsize);

    decode_bufsize = (bufsize / sbc_get_frame_length(&data->sbc) + 1 ) * sbc_get_codesize(&data->sbc);
    decode_buf = malloc (decode_bufsize);

    // read bluetooth
    readlen = read(data->fd, buf, bufsize);
    if (readlen == 0)
    {
      //data->command = IO_CMD_TERMINATE;
      debug_print("============= TERMINATE 1 ===============================================\n");
      continue;
    }
    else if (readlen < 0)
    {
      debug_print("============= TERMINATE 2 ===============================================\n");
      //continue;
        data->streamStatus = 0;
        snd_pcm_drop(data->pcm);
        snd_pcm_close(data->pcm);
        free(buf);
        free(pollfds);
        free(decode_buf);
        pthread_exit(0);
    }

    header = buf;
    payload = (struct rtp_payload*) ((uint8_t*) buf + sizeof(*header));

    void *p = buf + sizeof(*header) + sizeof(*payload);
    size_t to_decode = readlen - sizeof(*header) - sizeof(*payload);

    void *d = decode_buf;
    size_t to_write = decode_bufsize;

    while (to_decode > 0)
    {
      decoded = sbc_decode(&data->sbc,
                           p, to_decode,
                           d, to_write,
                           &written);

      if (decoded <= 0)
      {
        debug_print("SBC decoding error %zd\n", decoded);
        break;
      }

      (size_t) decoded <= to_decode;
      (size_t) decoded == sbc_get_frame_length(&data->sbc);

      (size_t) written == sbc_get_codesize(&data->sbc);

      p = (uint8_t*) p + decoded;
      to_decode -= decoded;
      d = (uint8_t*) d + written;
      to_write -= written;
    }

    //

//    r = poll(pollfds, n_pollfd, 0);
//    assert(r >= 0);
//    r = snd_pcm_poll_descriptors_revents(data->pcm, pollfds, n_pollfd, &revents);
//    assert(r == 0);
//    assert((revents & ~POLLOUT) == 0);
//    avail = snd_pcm_avail(data->pcm);
//    assert(avail >= 0);
//    r = snd_pcm_status(data->pcm, status);
//    assert(r == 0);

    /* This assertion fails from time to time. ALSA seems to be broken */
    //   assert(avail == (snd_pcm_sframes_t) snd_pcm_status_get_avail(status));
    //   printf("%lu %lu\n", (unsigned long) avail, (unsigned long) snd_pcm_status_get_avail(status));

//    snd_pcm_status_get_htstamp(status, &timestamp);
//    delay = snd_pcm_status_get_delay(status);
//    state = snd_pcm_status_get_state(status);

    //assert(!revents || avail > 0);

    sent_frames = 0;
    sent = 0;

    // write stdout
    do
    {
      sent_frames = snd_pcm_writei(data->pcm,
                                   (char *) decode_buf + sent,
                                   snd_pcm_bytes_to_frames(data->pcm, decode_bufsize - to_write - sent));
      if(sent_frames < 0)
      {
        //assert(sent_frames != -EAGAIN);
        if (sent_frames == -EPIPE)
          debug_print("%s: Buffer underrun! #######", "snd_pcm_writei");

        if (sent_frames == -ESTRPIPE)
          debug_print("%s: System suspended! #######", "snd_pcm_writei");

        if (snd_pcm_recover(data->pcm, sent_frames, 1) < 0)
        {
          debug_print("%s: RECOVER #######!", "snd_pcm_writei");
          break;
        }
      }
      sent += snd_pcm_frames_to_bytes(data->pcm, sent_frames);

      debug_print(">>> READ (%d) - WRITE (%d) - availPCM (%d) - THREAD (%d)\n", readlen, (decode_bufsize - to_write), avail, data->devId);
    }while (sent < (decode_bufsize - to_write) && timeout > 0 && data->command == IO_CMD_RUNNING);
   }
  }

  debug_print("FINISH\n");
  snd_pcm_drop(data->pcm);
  snd_pcm_close(data->pcm);
  free(buf);
  free(pollfds);
  free(decode_buf);
  pthread_exit(100);
}




/*****************//**
     * Main I/O thread function
     * This function will perform actual streaming of data.
     * It is simple: read the data from bluez' descriptor, and write to stdout!
     *
     * The rest is just management overhead.
     *
     * In output mode, the output (on stdout) will raw S16_LE, 44.1kHz, stereo/mono (depends on source).
     * See "endpoint_select_configuration" if you want to change this.
     * aplay -f cd can play this correctly
     *
     * In input mode, the input (on stdin) must be raw S16_LE, 48kHz, stereo/mono (depends on target).
     * See "endpoint_select_configuration" if you want to change this.
     * The encoding function is taken from pulseaudio 2.1 (simplified).
     *
     * @param [in] control-block for this thread.
     * @returns NULL
     *********************/
void *io_thread_run(void *ptr)
{
  io_thread_tcb_s *data = ptr;

  // prepare
  debug_print ("starting %p\n", ptr);
  pthread_mutex_lock (&data->mutex);

  // run
  while (1)
  {
    switch (data->command)
    {
    case IO_CMD_IDLE:
      debug_print ("\n IO_CMD_IDLE !!!\n");
      pthread_cond_wait (&data->cond, &data->mutex);
      break;

    case IO_CMD_RUNNING:
      debug_print ("\n IO_CMD_RUNNING !!!\n");
      run_sink_A2DP(data);
      break;

    case IO_CMD_RUN_HEADSET:
      debug_print ("\n IO_CMD_RUN_HEADSET !!!\n");
      run_source_A2DP(data);
      break;

    case IO_CMD_TERMINATE:
      debug_print ("terminate %p\n", ptr);
      goto end;
    }
  }

end:
  // cleanup
  sbc_finish(&data->sbc);
  pthread_mutex_unlock (&data->mutex);
  return NULL;
}


//////////////////////////////// MAIN ////////////////////////////////

int main(int argc, char** argv)
{
  // Control variables
  DBusConnection* system_bus;
  char *bt_object;	// bluetooth device objectpath
  io_thread_tcb_s *io_threads_table = NULL; // hashtable of io_threads
  int msg_waiting_time = -1; //default is wait forever

  // scratch variables
  DBusMessage *msg, *reply;
  DBusError err;

  parseConfigFile();

  // 0. check options
  const struct option options[] = {
  {"help",     no_argument, 0,  'h' },
  {"sink",     no_argument, 0,  's' },
  {"hfp",      no_argument, 0,  'p' },
  {"source",   no_argument, 0,  'o' },
  {"run-once", no_argument, 0,  'r' },
  {0,          0,           0,   0  }
  };

  int option, run_sink=0, run_source=0, run_hfp=0, argCounter = 1, interfaceNumber = 0;
  while ((option = getopt_long (argc, argv, "h", (const struct option *) &options, NULL)) != -1)
  {
    switch (option)
    {
    case 'h':
      fprintf (stderr,"Usage: a2dp-alsa [--sink|--source]\n");
      return 0;
    case 's':
      debug_print ("run sink\n");
      run_sink=1;
      break;
    case 'p':
      debug_print ("run hfp\n");
      run_hfp=1;
      break;
    case 'o':
      debug_print ("run source\n");
      run_source=1;
      break;
    case 'r':
      debug_print ("run once\n");
      run_once=1;
      msg_waiting_time = 1000; //1000 ms = 1 s
      break;
    case 'i':
      interfaceNumber = argCounter;
      debug_print ("selected interface (%d)\n", interfaceNumber);
      break;
    }
    argCounter++;
  }

  // 1. init - get bus and adapter
  debug_print ("daemon started\n");
  dbus_threads_init_default();
  dbus_error_init(&err);

  if (get_system_bus(&system_bus))
  {
    //if (!get_bluetooth_object(system_bus, argv[optind], &bt_object)) return 1;
    if(interfaceNumber != 0)
    {
      char result[100];
      strcpy(result,"/org/bluez/");
      strcat(result,argv[interfaceNumber]);
      bt_object = result;
    }
    else
    {
      bt_object = "/org/bluez/hci0";
    }
  } else return 1;

  debug_print ("Adaptor path : (%s)\n", bt_object);

  // 2. register endpoint

    //char *sinks = getSinks();

    run_source =  media_register_endpoint(system_bus, bt_object, A2DP_SOURCE_ENDPOINT, A2DP_SOURCE_UUID); // alsa --> bt
    run_sink   =  media_register_endpoint(system_bus, bt_object, A2DP_SINK_ENDPOINT, A2DP_SINK_UUID); // bt --> alsa
    run_sink   =  media_register_endpoint(system_bus, bt_object, A2DP_SINK_ENDPOINT_SECOND, A2DP_SINK_UUID); // bt --> alsa

  int deviceType = 0;

    dbus_bus_add_match (system_bus, "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'", &err);
    handle_dbus_error (&err, __FUNCTION__, __LINE__);

  // 4. main-loop
  while (!quit && dbus_connection_read_write (system_bus, msg_waiting_time))
  { //block
    while (!quit && (msg = dbus_connection_pop_message (system_bus)))
    {
      reply = NULL;

      if(dbus_message_is_signal(msg, "org.freedesktop.DBus.Properties", "PropertiesChanged")) // bt --> alsa
      {
        audiosource_property_changed (system_bus, msg, deviceType, &io_threads_table);
      }
      else if (dbus_message_is_method_call (msg, "org.bluez.MediaEndpoint1", "SetConfiguration"))
      {
        reply = endpoint_set_configuration (msg, &io_threads_table);
      }
      else if (dbus_message_is_method_call (msg, "org.bluez.MediaEndpoint1", "SelectConfiguration"))
      {
        reply = endpoint_select_configuration (msg);
      }
      else if(dbus_message_is_method_call (msg, "org.bluez.MediaEndpoint1", "ClearConfiguration"))
      {
        reply = endpoint_clear_configuration (msg, &io_threads_table, system_bus);
      }
      else if (dbus_message_is_method_call (msg, "org.bluez.MediaEndpoint1", "Release"))
      { reply = endpoint_release (msg); quit=1; }
      if (reply)
      {
        // send the reply
        dbus_connection_send(system_bus, reply, NULL);
        dbus_message_unref(reply);
      }
      dbus_message_unref(msg);
    }
  }

  // 5. destroy all existing I/O threads
  if (io_threads_table)
  {
    io_thread_tcb_s *p = io_threads_table, *next;
    do
    {
      next = p->hh.next;
      destroy_io_thread (p);
      p = next;
    } while (p && p != io_threads_table);
  }

  // 6. cleanup and exit
  dbus_connection_flush (system_bus);
  dbus_connection_unref (system_bus);
  debug_print ("a2dp ended\n\n");
  return 0;
}
