/* AYTHER QA altered-source marker: observation only, not an upstream SDL API.
   Included only by the private, disposable SDL build used by this experiment.
 */
#include "observer.h"
extern int AytherQaResamplerSupportLeft(void);
extern int AytherQaResamplerSupportRight(void);

static AytherQaConversionObserver ayther_qa_observer;
static void *ayther_qa_userdata;
static AytherQaSubmissionObserver ayther_qa_submission_observer;
static void *ayther_qa_submission_userdata;
static AytherQaResetObserver ayther_qa_reset_observer;
static void *ayther_qa_reset_userdata;
static AytherQaEndObserver ayther_qa_end_observer;
static void *ayther_qa_end_userdata;

__declspec(dllexport) void SDLCALL
AYTHER_QA_SetEndObserver(AytherQaEndObserver observer, void *userdata) {
  ayther_qa_end_userdata = userdata;
  ayther_qa_end_observer = observer;
}

static void AytherQaObserveEnd(SDL_AudioStream *stream) {
  if (ayther_qa_end_observer) {
    const AytherQaStreamEnd event = {stream, stream->src_spec,
                                     SDL_GetAudioQueueQueued(stream->queue),
                                     stream->resample_offset};
    ayther_qa_end_observer(ayther_qa_end_userdata, &event);
  }
}

__declspec(dllexport) void SDLCALL
AYTHER_QA_SetResetObserver(AytherQaResetObserver observer, void *userdata) {
  ayther_qa_reset_userdata = userdata;
  ayther_qa_reset_observer = observer;
}

static void AytherQaObserveReset(SDL_AudioStream *stream, size_t queued_before,
                                 Sint64 phase_before) {
  if (ayther_qa_reset_observer) {
    const AytherQaReset event = {
        stream,        stream->src_spec,
        queued_before, SDL_GetAudioQueueQueued(stream->queue),
        phase_before,  stream->resample_offset};
    ayther_qa_reset_observer(ayther_qa_reset_userdata, &event);
  }
}

__declspec(dllexport) void SDLCALL AYTHER_QA_SetSubmissionObserver(
    AytherQaSubmissionObserver observer, void *userdata) {
  ayther_qa_submission_userdata = userdata;
  ayther_qa_submission_observer = observer;
}

static void AytherQaObserveSubmission(SDL_AudioStream *stream,
                                      const SDL_AudioSpec *spec, int bytes,
                                      const void *data) {
  if (ayther_qa_submission_observer) {
    ayther_qa_submission_observer(ayther_qa_submission_userdata, stream, spec,
                                  bytes, data);
  }
}

__declspec(dllexport) void SDLCALL AYTHER_QA_SetConversionObserver(
    AytherQaConversionObserver observer, void *userdata) {
  ayther_qa_userdata = userdata;
  ayther_qa_observer = observer;
}

static void AytherQaObserveConversion(SDL_AudioStream *stream, int input_frames,
                                      int output_frames, int padding_frames,
                                      Sint64 rate, Sint64 phase_before,
                                      float gain, const void *output) {
  if (ayther_qa_observer) {
    const AytherQaConversion event = {stream,
                                      stream->input_spec,
                                      stream->dst_spec,
                                      input_frames,
                                      output_frames,
                                      padding_frames,
                                      rate ? AytherQaResamplerSupportLeft() : 0,
                                      rate ? AytherQaResamplerSupportRight()
                                           : 0,
                                      rate,
                                      phase_before,
                                      stream->resample_offset,
                                      gain,
                                      output};
    ayther_qa_observer(ayther_qa_userdata, &event);
  }
}
