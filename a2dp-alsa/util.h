#include "a2dp-codecs.h"	// from bluez - some sbc constants
#include "ipc.h"          // from bluez - some sbc constants
#include "rtp.h"          // from bluez - packet headers
#include "sbc/sbc.h"
#include <pthread.h>
#include <sbc/sbc.h>

typedef struct cards
{
    char name[20]; // 20 character array
    int  busy;
}cards;

typedef struct audio
{
    cards sinks[10];
    cards sources[10];
    int numOfSinks;
    int numOfSources;
}audio;


typedef struct
{
  // sync and command management
  pthread_cond_t cond;
  pthread_mutex_t mutex;
  pthread_t t_handle;       // thread handle
  pthread_t t_audio_handle; // audio stream thread handle

  volatile enum
  {
    IO_CMD_IDLE = 0,
    IO_CMD_RUNNING,
    IO_CMD_RUN_HEADSET,
    IO_CMD_TERMINATE,
    IO_CMD_INIT_PCM
  } command;

  enum
  {
    STATE_DISCONNECTED = 0,
    STATE_CONNECTED,
    STATE_PLAYING
  } prev_state;

  // transport_path - required to get fd and mtus
  char *transport_path;	// also the hash key
  char *dev_path;			// so that audiosource/sink event can find us

  // the actual fd and mtus for streaming
  int fd, read_mtu, write_mtu;
  int write; //false = read, true - write

  int devId;
  int streamStatus;
  int cardNumberUsed;
  snd_pcm_t *pcm;

  char *devType;

  // codec stuff
  a2dp_sbc_t cap;
  sbc_t sbc;

  DBusConnection *connData;

  // persistent stuff for encoding purpose
  uint16_t seq_num;   //cumulative packet number
  uint32_t timestamp; //timestamp

  //hashtable management
  UT_hash_handle hh;
} io_thread_tcb_s; //the I/O thread control block.
