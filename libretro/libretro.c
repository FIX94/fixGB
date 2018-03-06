
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <malloc.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <math.h>
#include "cpu.h"
#include "input.h"
#include "ppu.h"
#include "mem.h"
#include "apu.h"
#include "libretro.h"

static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;

#define VISIBLE_DOTS 160
#define VISIBLE_LINES 144

int gbEmuLoadGame(const char *filename);
void gbEmuMainLoop(void);
void gbEmuDeinit(void);
extern uint8_t inValReads[8];
extern uint32_t textureImage[0x5A00];
extern volatile bool emuRenderFrame;
extern const char *VERSION_STRING;

void memLoadSave()
{

}
void memSaveGame()
{

}
int audioInit()
{

}
void audioDeinit()
{

}
void audioSleep()
{

}

void retro_get_system_info(struct retro_system_info *info)
{
   info->library_name = "fixGB";
   info->library_version = VERSION_STRING + 6;
   info->need_fullpath = true;
   info->block_extract = false;
   info->valid_extensions = "gb|gbc|gbs";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->geometry.base_width  = VISIBLE_DOTS;
   info->geometry.base_height = VISIBLE_LINES;
   info->geometry.max_width   = VISIBLE_DOTS;
   info->geometry.max_height  = VISIBLE_LINES;
   info->geometry.aspect_ratio  = 0.0f;
   info->timing.fps           = 4194304.0 / 70224.0;
   info->timing.sample_rate   = (float)apuGetFrequency();
}

void retro_init(void)
{
   struct retro_log_callback log;

   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
}

void retro_deinit()
{
}

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}
void retro_set_audio_sample(retro_audio_sample_t cb) { }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}
void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}
void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {}

void retro_reset()
{
}

size_t retro_serialize_size(void)
{
   return 0;
}

bool retro_serialize(void *data, size_t size)
{
   return false;
}

bool retro_unserialize(const void *data, size_t size)
{
   return false;
}
void retro_cheat_reset()
{
}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
}


bool retro_load_game(const struct retro_game_info *info)
{
   struct retro_input_descriptor desc[] =
   {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },

      { 0 },
   };

   if (gbEmuLoadGame(info->path) != EXIT_SUCCESS)
      return false;

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
#ifdef USE_RGB565
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;

   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      log_cb(RETRO_LOG_ERROR, "RGB565 is not supported.\n");
      return false;
   }
#else
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;

   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      log_cb(RETRO_LOG_ERROR, "XRGB8888 is not supported.\n");
      return false;
   }
#endif

   return true;
}


bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info,
                             size_t num_info)

{
   return false;
}

void retro_unload_game()
{
   gbEmuDeinit();
}

unsigned retro_get_region()
{
   return RETRO_REGION_NTSC;
}

void *retro_get_memory_data(unsigned id)
{
   switch(id & RETRO_MEMORY_MASK)
   {
   case RETRO_MEMORY_SAVE_RAM:
      return Ext_Mem;
   }
   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   switch(id & RETRO_MEMORY_MASK)
   {
   case RETRO_MEMORY_SAVE_RAM:
      return sizeof(Ext_Mem);
   }
   return 0;
}

int audioUpdate()
{
#if 0
#if AUDIO_FLOAT
   static int16_t buffer[512 * 2];
   float* buffer_in = (float*)apuGetBuf();
   int16_t* out_ptr = buffer;
   int samples = apuGetBufSize() / sizeof(float);
   while (samples)
   {
#define CLAMP_16(x) (((x) > INT16_MAX)? INT16_MAX : ((x) < INT16_MIN)? INT16_MIN : (x))
      *(out_ptr++) = CLAMP_16((*buffer_in * 0x4000) - 0x2000);
      buffer_in++;
      samples--;
      if (out_ptr == &buffer[512 * 2])
      {
         audio_batch_cb(buffer, 512);
         out_ptr = buffer;
      }
   }

   if (out_ptr > buffer)
      audio_batch_cb(buffer, (out_ptr - buffer) / 2);
#else
   uint16_t* buffer_in = (uint16_t*)apuGetBuf();
   int samples = apuGetBufSize() / (2 * sizeof(uint16_t));
   while (samples > 512)
   {
     audio_batch_cb(buffer_in, 512);
     buffer_in += 1024;
     samples -= 512;
   }
   audio_batch_cb(buffer_in, samples);
#endif
#endif
   return 1;
}

void apuFrameEnd();
void audioFrameEnd(int samples)
{
#if AUDIO_FLOAT
#else
   uint16_t* buffer_in = (uint16_t*)apuGetBuf();
   while (samples > 512)
   {
     audio_batch_cb(buffer_in, 512);
     buffer_in += 1024;
     samples -= 512;
   }
   if(samples)
      audio_batch_cb(buffer_in, samples);
#endif
}

void retro_run()
{
   input_poll_cb();

   inValReads[BUTTON_A]      = !!input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
   inValReads[BUTTON_B]      = !!input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
   inValReads[BUTTON_SELECT] = !!input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT);
   inValReads[BUTTON_START]  = !!input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
   inValReads[BUTTON_RIGHT]  = !!input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT);
   inValReads[BUTTON_LEFT]   = !!input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
   inValReads[BUTTON_UP]     = !!input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
   inValReads[BUTTON_DOWN]   = !!input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);

   gbEmuMainLoop();

   video_cb(textureImage, VISIBLE_DOTS, VISIBLE_LINES, VISIBLE_DOTS * sizeof(uint32_t));
   apuFrameEnd();

   emuRenderFrame = false;
}

unsigned retro_api_version()
{
   return RETRO_API_VERSION;
}
