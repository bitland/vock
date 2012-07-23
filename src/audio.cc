#include "audio.h"
#include "common.h"

#include "node.h"
#include "node_buffer.h"

#include <AudioUnit/AudioUnit.h>
#include <string.h>
#include <unistd.h>

namespace vock {
namespace audio {

using namespace node;
using v8::HandleScope;
using v8::Handle;
using v8::Persistent;
using v8::Local;
using v8::Array;
using v8::String;
using v8::Number;
using v8::Value;
using v8::Arguments;
using v8::Object;
using v8::Null;
using v8::True;
using v8::False;
using v8::Function;
using v8::FunctionTemplate;
using v8::ThrowException;

static Persistent<String> ondata_sym;

#define CHECK(op, msg)\
    {\
      OSStatus st = 0;\
      if ((st = op) != noErr) {\
        char err[1024];\
        snprintf(err, sizeof(err), "%s - %d", msg, st);\
        ThrowException(String::New(err));\
        return NULL;\
      }\
    }

Audio::Audio() : in_unit_(NULL),
                 out_unit_(NULL),
                 in_buffer_(100 * 1024),
                 out_buffer_(100 * 1024) {
  // Initialize description
  memset(&desc_, 0, sizeof(desc_));

  // Setup input/output units
  in_unit_ = CreateAudioUnit(true);
  out_unit_ = CreateAudioUnit(false);

  // Setup async callbacks
  if (uv_async_init(uv_default_loop(), &in_async_, InputAsyncCallback)) {
    abort();
  }
  uv_unref(reinterpret_cast<uv_handle_t*>(&in_async_));

  // Setup mutexes
  if (uv_mutex_init(&in_mutex_)) {
    abort();
  }
  if (uv_mutex_init(&out_mutex_)) {
    abort();
  }

  // Init buffer list
  blist_ = reinterpret_cast<AudioBufferList*>(calloc(
      1, sizeof(&blist_) + desc_.mChannelsPerFrame * sizeof(AudioBuffer)));
  if (blist_ == NULL) abort();
  blist_->mNumberBuffers = 1;
  blist_->mBuffers[0].mNumberChannels = desc_.mChannelsPerFrame;
  blist_->mBuffers[0].mData = NULL;
  blist_->mBuffers[0].mDataByteSize = 0;
}


Audio::~Audio() {
  AudioUnitUninitialize(in_unit_);
  AudioUnitUninitialize(out_unit_);
  uv_close(reinterpret_cast<uv_handle_t*>(&in_async_), NULL);
  uv_mutex_destroy(&in_mutex_);
  uv_mutex_destroy(&out_mutex_);
  free(blist_);
}


AudioUnit Audio::CreateAudioUnit(bool is_input) {
  UInt32 enable = 1;
  UInt32 disable = 0;

  // Initialize Unit
  AudioComponentDescription au_desc;
  AudioComponent au_component;
  AudioComponentInstance unit;

  au_desc.componentType = kAudioUnitType_Output;
  au_desc.componentSubType = kAudioUnitSubType_HALOutput;
  au_desc.componentManufacturer = kAudioUnitManufacturer_Apple;
  au_desc.componentFlags = 0;
  au_desc.componentFlagsMask = 0;

  au_component = AudioComponentFindNext(NULL, &au_desc);
  if (au_component == NULL) {
    ThrowException(String::New("AudioComponentFindNext() failed"));
    return NULL;
  }

  CHECK(AudioComponentInstanceNew(au_component, &unit),
        "AudioComponentInstanceNew() failed")

  // Attach input/output
  CHECK(AudioUnitSetProperty(unit,
                             kAudioOutputUnitProperty_EnableIO,
                             kAudioUnitScope_Input,
                             kInputBus,
                             &enable,
                             sizeof(enable)),
        "Input: EnableIO failed")
  CHECK(AudioUnitSetProperty(unit,
                             kAudioOutputUnitProperty_EnableIO,
                             kAudioUnitScope_Output,
                             kOutputBus,
                             is_input ? &disable : &enable,
                             sizeof(enable)),
       "Output: EnableIO failed")

  // Set input device
  if (is_input) {
    UInt32 input_size = sizeof(AudioDeviceID);
    AudioDeviceID input;
    CHECK(AudioHardwareGetProperty(kAudioHardwarePropertyDefaultInputDevice,
                                   &input_size,
                                   &input),
          "Failed to get input device")
    CHECK(AudioUnitSetProperty(unit,
                               kAudioOutputUnitProperty_CurrentDevice,
                               kAudioUnitScope_Global,
                               kOutputBus,
                               &input,
                               sizeof(input)),
          "Failed to set input device")
  }

  // Setup callbacks
  AURenderCallbackStruct callback;

  if (is_input) {
    callback.inputProc = InputCallback;
    callback.inputProcRefCon = this;
    CHECK(AudioUnitSetProperty(unit,
                               kAudioOutputUnitProperty_SetInputCallback,
                               kAudioUnitScope_Global,
                               kOutputBus,
                               &callback,
                               sizeof(callback)),
          "Input: set callback failed")
  } else {
    callback.inputProc = OutputCallback;
    callback.inputProcRefCon = this;
    CHECK(AudioUnitSetProperty(unit,
                               kAudioUnitProperty_SetRenderCallback,
                               kAudioUnitScope_Global,
                               kOutputBus,
                               &callback,
                               sizeof(callback)),
          "Output: set callback failed")
  }

  // Set format
  if (is_input) {
    UInt32 size = sizeof(desc_);
    CHECK(AudioUnitGetProperty(unit,
                               kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Input,
                               kInputBus,
                               &desc_,
                               &size),
          "Input: get StreamFormat failed")
  }
  CHECK(AudioUnitSetProperty(unit,
                             kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Output,
                             kInputBus,
                             &desc_,
                             sizeof(desc_)),
        "Input: set StreamFormat failed")
  CHECK(AudioUnitSetProperty(unit,
                             kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input,
                             kOutputBus,
                             &desc_,
                             sizeof(desc_)),
        "Output: set StreamFormat failed")

  // Some wierd options
  CHECK(AudioUnitSetProperty(unit,
                             kAudioUnitProperty_ShouldAllocateBuffer,
                             kAudioUnitScope_Output,
                             kInputBus,
                             &disable,
                             sizeof(disable)),
        "Input: ShouldAllocateBuffer failed")

  CHECK(AudioUnitInitialize(unit), "AudioUnitInitialized() failed")

  return unit;
}


Handle<Value> Audio::New(const Arguments& args) {
  HandleScope scope;

  // Second argument is in msec
  Audio* a = new Audio();
  a->Wrap(args.Holder());

  a->handle_->Set(String::NewSymbol("rate"), Number::New(a->desc_.mSampleRate));
  a->handle_->Set(String::NewSymbol("channels"),
                  Number::New(a->desc_.mChannelsPerFrame));

  return scope.Close(args.This());
}


Handle<Value> Audio::Start(const Arguments& args) {
  HandleScope scope;
  Audio* a = ObjectWrap::Unwrap<Audio>(args.This());

  OSStatus st;

  st = AudioOutputUnitStart(a->in_unit_);
  if (st) {
    return scope.Close(ThrowException(String::New(
        "Failed to start unit!")));
  }
  st = AudioOutputUnitStart(a->out_unit_);
  if (st) {
    return scope.Close(ThrowException(String::New(
        "Failed to start unit!")));
  }
  uv_ref(reinterpret_cast<uv_handle_t*>(&a->in_async_));
  a->Ref();

  return scope.Close(Null());
}


Handle<Value> Audio::Stop(const Arguments& args) {
  HandleScope scope;
  Audio* a = ObjectWrap::Unwrap<Audio>(args.This());

  OSStatus st;

  st = AudioOutputUnitStop(a->in_unit_);
  if (st) {
    return scope.Close(ThrowException(String::New(
        "Failed to stop unit!")));
  }
  st = AudioOutputUnitStop(a->out_unit_);
  if (st) {
    return scope.Close(ThrowException(String::New(
        "Failed to stop unit!")));
  }
  uv_unref(reinterpret_cast<uv_handle_t*>(&a->in_async_));
  a->Unref();

  return scope.Close(Null());
}


Handle<Value> Audio::Enqueue(const Arguments& args) {
  HandleScope scope;
  Audio* a = ObjectWrap::Unwrap<Audio>(args.This());

  if (args.Length() < 1 || !Buffer::HasInstance(args[0])) {
    return scope.Close(ThrowException(String::New(
        "First argument should be a Buffer!")));
  }

  char* buff = Buffer::Data(args[0].As<Object>());
  size_t size = Buffer::Length(args[0].As<Object>());

  uv_mutex_lock(&a->out_mutex_);
  char* data = a->out_buffer_.Produce(size);
  memcpy(data, buff, size);
  uv_mutex_unlock(&a->out_mutex_);

  return scope.Close(Null());
}


OSStatus Audio::InputCallback(void* arg,
                              AudioUnitRenderActionFlags* flags,
                              const AudioTimeStamp* ts,
                              UInt32 bus,
                              UInt32 frame_count,
                              AudioBufferList* data) {
  Audio* a = reinterpret_cast<Audio*>(arg);

  uv_mutex_lock(&a->in_mutex_);

  // Setup buffer list
  UInt32 size = frame_count * a->desc_.mBytesPerFrame;
  a->blist_->mBuffers[0].mDataByteSize = size;
  a->blist_->mBuffers[0].mData = a->in_buffer_.Produce(size);

  // Write received data to buffer list
  OSStatus st;
  st = AudioUnitRender(a->in_unit_, flags, ts, bus, frame_count, a->blist_);
  if (st) {
    fprintf(stderr, "%d\n", st);
    abort();
  }

  uv_async_send(&a->in_async_);
  uv_mutex_unlock(&a->in_mutex_);

  return 0;
}


void Audio::InputAsyncCallback(uv_async_t* async, int status) {
  HandleScope scope;
  Audio* a = container_of(async, Audio, in_async_);

  uv_mutex_lock(&a->in_mutex_);
  if (a->in_buffer_.Size() == 0) return;
  Buffer* in = Buffer::New(a->in_buffer_.Size());
  a->in_buffer_.Flush(Buffer::Data(in));
  uv_mutex_unlock(&a->in_mutex_);

  Handle<Value> argv[1] = { in->handle_ };
  MakeCallback(a->handle_, ondata_sym, 1, argv);
}


OSStatus Audio::OutputCallback(void* arg,
                               AudioUnitRenderActionFlags* flags,
                               const AudioTimeStamp* ts,
                               UInt32 bus,
                               UInt32 frame_count,
                               AudioBufferList* data) {
  Audio* a = reinterpret_cast<Audio*>(arg);

  char* buff = reinterpret_cast<char*>(data->mBuffers[0].mData);
  UInt32 size = data->mBuffers[0].mDataByteSize;

  // Put available bytes from buffer
  uv_mutex_lock(&a->out_mutex_);
  size_t written = a->out_buffer_.Fill(buff, size);
  uv_mutex_unlock(&a->out_mutex_);

  // Fill rest with zeroes
  if (written < size) {
    memset(buff + written, 0, size - written);
  }

  return noErr;
}


void Audio::Init(Handle<Object> target) {
  HandleScope scope;

  ondata_sym = Persistent<String>::New(String::NewSymbol("ondata"));

  Local<FunctionTemplate> t = FunctionTemplate::New(Audio::New);

  t->InstanceTemplate()->SetInternalFieldCount(1);
  t->SetClassName(String::NewSymbol("Audio"));

  NODE_SET_PROTOTYPE_METHOD(t, "start", Audio::Start);
  NODE_SET_PROTOTYPE_METHOD(t, "stop", Audio::Stop);
  NODE_SET_PROTOTYPE_METHOD(t, "enqueue", Audio::Enqueue);

  target->Set(String::NewSymbol("Audio"), t->GetFunction());
}

} // namespace audio
} // namespace vock
