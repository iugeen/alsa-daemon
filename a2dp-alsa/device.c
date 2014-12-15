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
#include "device.h"

#define pa_streq(a,b) (!strcmp((a),(b)))
#define PCM_DEVICE        "plughw:1,0"
#define PCM_DEVICE_SECOND "A2DP_playback_1"
#define true 1
#define false 0
#define debug_print(...) (fprintf (stderr, __VA_ARGS__))

void parseConfigFile(audio *audioCards)
{
   char *ch = NULL;
   char *line = NULL;
   char *confFile = "/var/pcm.conf";
   size_t len = 0;
   ssize_t read;
   int sinkNumber = 0,  sourceNumber = 0;

    debug_print ("read configuration file (%s) .........\n", confFile);
    FILE* file = fopen(confFile, "r");
    if ( 0 != file )
    {
        while ((read = getline(&line, &len, file)) != -1)
        {
            size_t ln = strlen(line) - 1;
            if (line[ln] == '\n')
                line[ln] = '\0';

            ch = strtok(line, "=");
            if(strcmp(ch, "sink") == 0)
            {
                ch = strtok(NULL, "=");
                strcpy(audioCards->sinks[sinkNumber].name, ch);
                audioCards->numOfSinks = sinkNumber;
                audioCards->sinks[sinkNumber].busy = 0;
                sinkNumber++;
            }
            else if(strcmp(ch, "source") == 0)
            {
                ch = strtok(NULL, "=");
                strcpy(audioCards->sources[sourceNumber].name, ch);
                audioCards->numOfSources = sourceNumber;
                audioCards->sources[sourceNumber].busy = 0;
                sourceNumber++;
            }
        }
        fclose(file);
    }
    else
    {
        debug_print("please create config file (/var/pcm.conf)\n");
    }
}

void run_sink_A2DP(io_thread_tcb_s *data, audio *cardName)
{
  int r;
  snd_pcm_hw_params_t *hwparams;
  snd_pcm_sw_params_t *swparams;
  snd_pcm_status_t *status;
  unsigned rate = 44100;
  unsigned periods = 2;
  snd_pcm_uframes_t boundary, buffer_size = 44100 / 10; /* 100s - 44100/10 */
  int dir = 1;
//  struct pollfd *pollfds;
//  int n_pollfd;
  int timeout;
  size_t bufsize, decode_bufsize;
  size_t written;
  size_t decoded;
  snd_pcm_sframes_t sent_frames = 0;
  int sent = 0;
  struct pollfd pollin = { data->fd, POLLIN, 0 };
  char *cardname; // = malloc(sizeof(char));
  int i = 0;

  if(data->streamStatus == 0 || data->streamStatus == 2)
  {
    switch (data->cap.frequency)
    {
    case BT_SBC_SAMPLING_FREQ_16000:
      data->sbc.frequency = SBC_FREQ_16000;
      break;
    case BT_SBC_SAMPLING_FREQ_32000:
      data->sbc.frequency = SBC_FREQ_32000;
      break;
    case BT_SBC_SAMPLING_FREQ_44100:
      data->sbc.frequency = SBC_FREQ_44100;
      break;
    case BT_SBC_SAMPLING_FREQ_48000:
      data->sbc.frequency = SBC_FREQ_48000;
      break;
    default:
      fprintf (stderr, "No supported frequency");
    }

    switch (data->cap.channel_mode)
    {
    case BT_A2DP_CHANNEL_MODE_MONO:
      data->sbc.mode = SBC_MODE_MONO;
      break;
    case BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL:
      data->sbc.mode = SBC_MODE_DUAL_CHANNEL;
      break;
    case BT_A2DP_CHANNEL_MODE_STEREO:
      data->sbc.mode = SBC_MODE_STEREO;
      break;
    case BT_A2DP_CHANNEL_MODE_JOINT_STEREO:
      data->sbc.mode = SBC_MODE_JOINT_STEREO;
      break;
    default:
      fprintf (stderr, "No supported channel_mode");
    }

    switch (data->cap.allocation_method)
    {
    case BT_A2DP_ALLOCATION_SNR:
      data->sbc.allocation = SBC_AM_SNR;
      break;
    case BT_A2DP_ALLOCATION_LOUDNESS:
      data->sbc.allocation = SBC_AM_LOUDNESS;
      break;
    default:
      fprintf (stderr, "No supported allocation");
    }

    switch (data->cap.subbands)
    {
    case BT_A2DP_SUBBANDS_4:
      data->sbc.subbands = SBC_SB_4;
      break;
    case BT_A2DP_SUBBANDS_8:
      data->sbc.subbands = SBC_SB_8;
      break;
    default:
      fprintf (stderr, "No supported subbands");
    }

    switch (data->cap.block_length)
    {
    case BT_A2DP_BLOCK_LENGTH_4:
      data->sbc.blocks = SBC_BLK_4;
      break;
    case BT_A2DP_BLOCK_LENGTH_8:
      data->sbc.blocks = SBC_BLK_8;
      break;
    case BT_A2DP_BLOCK_LENGTH_12:
      data->sbc.blocks = SBC_BLK_12;
      break;
    case BT_A2DP_BLOCK_LENGTH_16:
      data->sbc.blocks = SBC_BLK_16;
      break;
    default:
      fprintf (stderr, "No supported block length");
    }

    data->sbc.bitpool = data->cap.max_bitpool;

    snd_pcm_hw_params_alloca(&hwparams);
    snd_pcm_sw_params_alloca(&swparams);
    snd_pcm_status_alloca(&status);

    i = 0;

    for(i; i<= audioCards->numOfSinks; i++)
    {
        if(audioCards->sinks[i].busy == 0)
        {
          audioCards->sinks[i].busy = 1;
          cardname = cardName->sinks[i].name;
          data->cardNumberUsed = i;
          break;
        }
    }

    debug_print ("\n INIT PCM : (%s)!!!\n", cardName->sinks[i].name);

    r = snd_pcm_open(&data->pcm, cardName->sinks[data->cardNumberUsed].name, SND_PCM_STREAM_PLAYBACK, 0);
    assert(r == 0);

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
//    n_pollfd = snd_pcm_poll_descriptors_count(data->pcm);
//    assert(n_pollfd > 0);
//    pollfds = malloc(sizeof(struct pollfd) * n_pollfd);
//    assert(pollfds);
//    r = snd_pcm_poll_descriptors(data->pcm, pollfds, n_pollfd);
//    assert(r == n_pollfd);

    printf("Starting. Buffer size is %u frames\n", (unsigned int) buffer_size);
    data->command = IO_CMD_RUNNING;
    data->streamStatus = 1;
  }

  ssize_t readlen=0;
  size_t to_decode;
  size_t to_write;

  while(1)
  {
    struct rtp_payload *payload;
    const void *p;
    void *d;
    void *buf, *decode_buf;
    struct rtp_header *header;

    timeout = poll(&pollin, 1, 500); //delay 1s to allow others to update our state
    if (timeout == 0)
    {
      debug_print("..continue.(%d)\n",timeout);
      data->streamStatus = 2;
      data->command = IO_CMD_IDLE;
      goto cleanup;
    }
    else if (timeout < 0)
    {
      debug_print("..poll...(%d)\n",timeout);
      data->command = IO_CMD_IDLE;
      goto cleanup;
    }

    // for(;;) ??
    // prepare buffer
    bufsize = 2 * data->read_mtu;
    buf = malloc(bufsize);

    decode_bufsize = (bufsize / sbc_get_frame_length(&data->sbc) + 1 ) * sbc_get_codesize(&data->sbc);
    decode_buf = malloc (decode_bufsize);

    header = buf;
    payload = (struct rtp_payload*) ((uint8_t*) buf + sizeof(*header));

    if((readlen = read(data->fd, buf, bufsize)) < 0)
    {
      if (errno == EINTR)
        continue;
    }

    if (readlen == 0)
    {
      debug_print("=== TERMINATE 1 ===\n");
      debug_print("FINISH\n");
      data->command = IO_CMD_TERMINATE;
      goto cleanup;
    }
    else if (readlen < 0 && errno == EINTR)
    {
      debug_print("== CONTINUE 2 ==\n");
      continue;
    }
    else if (readlen < 0 && errno == EAGAIN)
    {
      debug_print("== TERMINATE 2 ==\n");
      data->streamStatus = 2;
      data->command = IO_CMD_IDLE;
      debug_print("FINISH\n");
      goto cleanup;
    }

    p = buf + sizeof(*header) + sizeof(*payload);
    to_decode = readlen - sizeof(*header) - sizeof(*payload);

    d = decode_buf;
    to_write = decode_bufsize;

    while (to_decode > 0)
    {
      decoded = sbc_decode(&data->sbc,
                           p, to_decode,
                           d, to_write,
                           &written);

      if (decoded <= 0)
      {
        debug_print("SBC decoding error %zd\n", decoded);
        goto cleanup;
      }

      (size_t) decoded <= to_decode;
      (size_t) decoded == sbc_get_frame_length(&data->sbc);

      (size_t) written == sbc_get_codesize(&data->sbc);

      p = (uint8_t*) p + decoded;
      to_decode -= decoded;
      d = (uint8_t*) d + written;
      to_write -= written;
    }

    sent = 0;
//    do
//    {
//      sent_frames = snd_pcm_writei(data->pcm,
//                                   (const uint8_t*) decode_buf + sent,
//                                   snd_pcm_bytes_to_frames(data->pcm, decode_bufsize - to_write - sent));
//      free(decode_buf);
//      if(sent_frames < 0)
//      {
//        assert(sent_frames != -EAGAIN);
//        if (sent_frames == -EPIPE)
//          debug_print("%s: Buffer underrun! ", "snd_pcm_writei");

//        if (sent_frames == -ESTRPIPE)
//          debug_print("%s: System suspended! ", "snd_pcm_writei");

//        if (snd_pcm_recover(data->pcm, sent_frames, 1) < 0)
//        {
//          debug_print("%s: RECOVER !", "snd_pcm_writei");
//          data->streamStatus = 2;
//          data->command = IO_CMD_IDLE;
//          goto cleanup;
//        }
//      }
//      sent += snd_pcm_frames_to_bytes(data->pcm, sent_frames);
//      //debug_print(">>> READ (%d) - WRITE (%d) - THREAD (%d)\n", readlen, (decode_bufsize - to_write), data->devId);
//    }while (sent < (decode_bufsize - to_write) && timeout > 0 && data->command == IO_CMD_RUNNING);


    free(buf);
  }

cleanup:
  snd_pcm_drop(data->pcm);
  snd_pcm_close(data->pcm);

  audioCards->sinks[data->cardNumberUsed].busy = 0;

  debug_print("FINISH audio stream ...\n");
}






void run_source_A2DP(io_thread_tcb_s *data, audio *cardName)
{
  //snd_pcm_t *data->pcm;
  void *bufHEADSET, *encode_buf_HEADSET;
  size_t bufsize_HEADSET, encode_bufsize_HEADSET;
  struct pollfd pollout = { data->fd, POLLOUT, 0 };
  int timeout_HEADSET;

  int r;
  int dir = 1;
  snd_pcm_hw_params_t *hwparams;
  snd_pcm_sw_params_t *swparams;
  snd_pcm_status_t *status;
  unsigned rate = 44100;
  unsigned periods = 2;
  snd_pcm_uframes_t boundary, buffer_size = 44100 / 10; /* 100s - 44100/10 */
  int i = 0;

  if(data->streamStatus == 0 || data->streamStatus == 2)
  {
    switch (data->cap.frequency)
    {
    case BT_SBC_SAMPLING_FREQ_16000:
      data->sbc.frequency = SBC_FREQ_16000;
      break;
    case BT_SBC_SAMPLING_FREQ_32000:
      data->sbc.frequency = SBC_FREQ_32000;
      break;
    case BT_SBC_SAMPLING_FREQ_44100:
      data->sbc.frequency = SBC_FREQ_44100;
      break;
    case BT_SBC_SAMPLING_FREQ_48000:
      data->sbc.frequency = SBC_FREQ_48000;
      break;
    default:
      fprintf (stderr, "No supported frequency");
    }

    switch (data->cap.channel_mode)
    {
    case BT_A2DP_CHANNEL_MODE_MONO:
      data->sbc.mode = SBC_MODE_MONO;
      break;
    case BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL:
      data->sbc.mode = SBC_MODE_DUAL_CHANNEL;
      break;
    case BT_A2DP_CHANNEL_MODE_STEREO:
      data->sbc.mode = SBC_MODE_STEREO;
      break;
    case BT_A2DP_CHANNEL_MODE_JOINT_STEREO:
      data->sbc.mode = SBC_MODE_JOINT_STEREO;
      break;
    default:
      fprintf (stderr, "No supported channel_mode");
    }

    switch (data->cap.allocation_method)
    {
    case BT_A2DP_ALLOCATION_SNR:
      data->sbc.allocation = SBC_AM_SNR;
      break;
    case BT_A2DP_ALLOCATION_LOUDNESS:
      data->sbc.allocation = SBC_AM_LOUDNESS;
      break;
    default:
      fprintf (stderr, "No supported allocation");
    }

    switch (data->cap.subbands)
    {
    case BT_A2DP_SUBBANDS_4:
      data->sbc.subbands = SBC_SB_4;
      break;
    case BT_A2DP_SUBBANDS_8:
      data->sbc.subbands = SBC_SB_8;
      break;
    default:
      fprintf (stderr, "No supported subbands");
    }

    switch (data->cap.block_length)
    {
    case BT_A2DP_BLOCK_LENGTH_4:
      data->sbc.blocks = SBC_BLK_4;
      break;
    case BT_A2DP_BLOCK_LENGTH_8:
      data->sbc.blocks = SBC_BLK_8;
      break;
    case BT_A2DP_BLOCK_LENGTH_12:
      data->sbc.blocks = SBC_BLK_12;
      break;
    case BT_A2DP_BLOCK_LENGTH_16:
      data->sbc.blocks = SBC_BLK_16;
      break;
    default:
      fprintf (stderr, "No supported block length");
    }

    data->sbc.bitpool = data->cap.max_bitpool;

    data->command = IO_CMD_RUN_HEADSET;
    data->streamStatus = 1;

    snd_pcm_hw_params_alloca(&hwparams);
    snd_pcm_sw_params_alloca(&swparams);
    snd_pcm_status_alloca(&status);

    debug_print ("write to bt (to fd - %d)\n", data->fd);

    i = 0;

    for(i; i<= audioCards->numOfSources; i++)
    {
        if(audioCards->sources[i].busy == 0)
        {
          audioCards->sources[i].busy = 1;
          data->cardNumberUsed = i;
          break;
        }
    }

    debug_print ("\n INIT PCM : (%s)!!!\n", cardName->sources[data->cardNumberUsed].name);

    r = snd_pcm_open(&data->pcm, cardName->sources[data->cardNumberUsed].name, SND_PCM_STREAM_CAPTURE, 0);
    assert(r == 0);
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
    r = snd_pcm_sw_params_set_avail_min(data->pcm, swparams, 0);
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
  }

  // stream
  while (1)
  {
    // get buffers
    encode_bufsize_HEADSET = data->write_mtu;
    encode_buf_HEADSET = malloc (encode_bufsize_HEADSET);

    bufsize_HEADSET = (encode_bufsize_HEADSET / sbc_get_frame_length(&data->sbc)) * // max frames allowed in a packet
        sbc_get_codesize(&data->sbc); // ensure all of our source will fit in a single packet
    bufHEADSET = malloc (bufsize_HEADSET);

    //debug_print ("encode_buf %d buf %d", encode_bufsize_HEADSET, bufsize_HEADSET);

    ssize_t readlen;
    int persize = bufsize_HEADSET;
    snd_pcm_sframes_t received_frames_HEADSET = 0;
    int received = 0;
    struct rtp_header *header;
    struct rtp_payload *payload;
    size_t nbytes;

    do{
      received_frames_HEADSET = snd_pcm_readi(data->pcm,
                                              (char *)bufHEADSET + received,
                                              snd_pcm_bytes_to_frames(data->pcm,persize - received));
      if(received_frames_HEADSET < 0)
      {
        //debug_print(">> NO AUDIO STREAM FROM PCM <<");
        break;
      }

      received += snd_pcm_frames_to_bytes(data->pcm, received_frames_HEADSET);
    } while (received < persize);

    header = encode_buf_HEADSET;
    payload = (struct rtp_payload*) ((uint8_t*) encode_buf_HEADSET + sizeof(*header));

    const void *p = bufHEADSET;
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

      if (encoded <= 0)
      {
        //debug_print ("SBC encoding error %zd\n", encoded);
        break; // make do with what have
      }

      p = (const uint8_t*) p + encoded;
      to_encode -= encoded;
      d = (uint8_t*) d + written;
      to_write -= written;

      frame_count++;
    }

    // encapsulate it in a2dp RTP packets
    /* write it to the fifo */
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
    if (!nbytes)
    {
      //debug_print ("nbytes: %zu\n", nbytes);
      break; // don't write if there is nothing to write
    }

    // wait until bluetooth is ready
    while (data->command == IO_CMD_RUN_HEADSET)
    {
      timeout_HEADSET = poll(&pollout, 1, 1000); //delay 1s to allow others to update our state
      if (timeout_HEADSET == 0)
      {
        debug_print ("flush bluetooth\n");
        continue;
      }
      if (timeout_HEADSET < 0)
      {
        fprintf (stderr, "bt_write/bluetooth: %d\n", errno);
      }
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
  debug_print ("=FINISH=\n");
  snd_pcm_close(data->pcm);
  free(bufHEADSET);
  free(encode_buf_HEADSET);
  audioCards->sources[data->cardNumberUsed].busy = 0;
}
