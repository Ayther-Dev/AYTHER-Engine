#ifndef AYTHER_QA_CONVERSION_OBSERVER_H
#define AYTHER_QA_CONVERSION_OBSERVER_H

#include <SDL3/SDL.h>

/* Test-only observer ABI. Register before using streams; unregister after
   destruction. Borrowed buffers expire when the callback returns. Never mutate
   stream or PCM. */
typedef struct AytherQaConversion {
  SDL_AudioStream *stream;
  SDL_AudioSpec source;
  SDL_AudioSpec destination;
  int input_frames;
  int output_frames;
  int padding_frames;
  int support_left;
  int support_right;
  Sint64 rate;
  Sint64 phase_before;
  Sint64 phase_after;
  float gain;
  const void *output;
} AytherQaConversion;

typedef void(SDLCALL *AytherQaConversionObserver)(void *,
                                                  const AytherQaConversion *);
typedef void(SDLCALL *AytherQaSetConversionObserver)(AytherQaConversionObserver,
                                                     void *);

typedef void(SDLCALL *AytherQaSubmissionObserver)(void *, SDL_AudioStream *,
                                                  const SDL_AudioSpec *, int,
                                                  const void *);
typedef void(SDLCALL *AytherQaSetSubmissionObserver)(AytherQaSubmissionObserver,
                                                     void *);

typedef struct AytherQaReset {
  SDL_AudioStream *stream;
  SDL_AudioSpec source;
  size_t queued_bytes_before;
  size_t queued_bytes_after;
  Sint64 phase_before;
  Sint64 phase_after;
} AytherQaReset;
typedef void(SDLCALL *AytherQaResetObserver)(void *, const AytherQaReset *);
typedef void(SDLCALL *AytherQaSetResetObserver)(AytherQaResetObserver, void *);

typedef struct AytherQaStreamEnd {
  SDL_AudioStream *stream;
  SDL_AudioSpec source;
  size_t queued_bytes;
  Sint64 phase;
} AytherQaStreamEnd;
typedef void(SDLCALL *AytherQaEndObserver)(void *, const AytherQaStreamEnd *);
typedef void(SDLCALL *AytherQaSetEndObserver)(AytherQaEndObserver, void *);

#endif
