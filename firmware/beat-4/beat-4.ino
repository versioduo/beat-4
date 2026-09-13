#include <V2Buttons.h>
#include <V2Device.h>
#include <V2LED.h>
#include <V2MIDI.h>
#include <V2PowerSupply.h>
#include <V2Solenoids.h>

namespace {
  namespace Setup {
    namespace Button {
      constexpr uint16_t led{0};
      constexpr uint16_t size{2};
    }

    namespace Ports {
      constexpr uint16_t led{2};
      constexpr uint16_t size{4};
    }
  }

  V2Device::Info                                          Info{V2DeviceInfo("com.versioduo.beat-4", 1, "versioduo:samd:beat-4")};
  V2LED::WS2812<Setup::Button::size + Setup::Ports::size> LED(PIN_LED_WS2812, sercom1, SPI_PAD_0_SCK_1, PIO_SERCOM);
  V2MIDI::SerialDevice                                    MIDISerial(&SerialMIDI, "serial");
  V2Base::Timer::PWM                                      PWM(0, 20000);
  V2Base::Analog::ADC                                     ADC(0);

  class Power : public V2PowerSupply {
  public:
    constexpr Power() : V2PowerSupply({.min{15}, .max{30}}) {}

    void begin() {
      pinMode(PIN_POWER_ENABLE, OUTPUT);
      digitalWrite(PIN_POWER_ENABLE, LOW);
    }

  private:
    float handleMeasurement() override {
      const uint8_t id      = V2Base::Analog::ADC::getID(PIN_VOLTAGE_SENSE);
      const uint8_t channel = V2Base::Analog::ADC::getChannel(PIN_VOLTAGE_SENSE);
      return 36.f * ADC.readChannel(channel);
    }

    void handleOn() override {
      digitalWrite(PIN_POWER_ENABLE, HIGH);
    }

    void handleOff() override {
      digitalWrite(PIN_POWER_ENABLE, LOW);
    }

    void handleNotify(float voltage) override {
      // Loss of power or requests to switch-on without a power connection show yellow LEDs.
      if (voltage < config.min) {
        LED.flash({V2Colour::Yellow, 1, 0.5}, 0.5);
        return;
      }

      // Over-voltage shows red LEDs.
      if (voltage > config.max) {
        LED.flash({V2Colour::Red, 1, 1}, 0.5);
        return;
      }

      // A bar of green LEDs show the voltage.
      float fraction{voltage / float(config.max)};
      auto  n{uint8_t(std::ceilf(float(Setup::Ports::size) * fraction))};
      LED.flash({V2Colour::Green, 1, 0.5}, 0.5, 2, n);
    }
  } Power;

  class Solenoids : public V2Solenoids<Setup::Ports::size> {
  public:
    Solenoids() :
      V2Solenoids({
        .current{.max{3}},
        .resistance{.min{2}, .max{100}},
        .fade{.inSec{0.35}, .outSec{0.35}},
        .hold{.peakUsec{100 * 1000}, .fraction{0.5}},
      }) {}

    auto setPower(PowerState state) -> bool override {
      switch (state) {
        case V2Solenoids::PowerState::On:
          digitalWrite(PIN_POWER_ENABLE, HIGH);
          break;

        case V2Solenoids::PowerState::Off:
          digitalWrite(PIN_POWER_ENABLE, LOW);
          break;
      }

      return true;
    }

    auto readVoltage() -> float override {
      return Power.getVoltage();
    }

    auto readCurrent() -> float override {
      auto id{V2Base::Analog::ADC::getID(PIN_CURRENT_SENSE)};
      auto channel{V2Base::Analog::ADC::getChannel(PIN_CURRENT_SENSE)};
      auto voltage{3.3f * ADC.readChannel(channel)};

      // Current sense ratio 3900, 1350 Ω sense resistor, 9.5 A maximum range.
      return voltage / 1350.f * 3900.f;
    }

    // One single port at a time is connected to 3.3V. The load and a 100Ω resistor
    // form a voltage divider.
    auto readResistanceVoltage() -> float override {
      auto id{V2Base::Analog::ADC::getID(PIN_RESISTANCE_SENSE)};
      auto channel{V2Base::Analog::ADC::getChannel(PIN_RESISTANCE_SENSE)};
      return 3.3f * ADC.readChannel(channel);
    }

    auto setLED(LEDMode state, uint8_t port = 0, float value = -1) -> void override {
      if (LED.rainbow())
        return;

      switch (state) {
        case LEDMode::Off:
          LED.brightness(0, Setup::Ports::led + port);
          break;

        case LEDMode::Initialize:
          LED.hsv({V2Colour::Cyan, 1, 0.25}, Setup::Button::led, Setup::Button::size);
          break;

        case LEDMode::Ready:
          LED.hsv({V2Colour::Orange, 1, 0.25}, Setup::Button::led, Setup::Button::size);
          break;

        case LEDMode::Resistance:
          // Map the fraction of the configured resistance range from cyan to magenta.
          LED.hsv({V2Colour::Cyan + (120.f * value), 1, 0.15}, Setup::Ports::led + port);
          break;

        case LEDMode::Power: {
          auto fraction{powf(value / 100.f, 8)};
          LED.hsv({V2Colour::Orange, 1, 0.3f + (0.3f * fraction)}, Setup::Ports::led + port);
        } break;

        case LEDMode::ShortCircuit:
          LED.hsv({V2Colour::Red, 1, 1}, Setup::Ports::led + port);
          break;

        case LEDMode::OverCurrent:
          LED.flash({V2Colour::Magenta, 1, 0.5}, 0.2);
          break;
      }
    }

    auto setPWMDuty(uint8_t port, float duty) -> void override {
      PWM.setDuty(PIN_PWM_CHANNEL + port, duty);
    }
  } Solenoids;

  // Config, written to EEPROM.
  constexpr struct Configuration {
    uint8_t channel{};
    uint8_t reserved[128]{};

    struct {
      uint8_t note;
      struct {
        float watts{2};
        float seconds{0.1};
      } min;

      struct {
        float watts{6};
        float seconds{0.05};
      } max;

      bool fadeIn{};
      bool fadeOut{};

      uint8_t reserved[128]{};
    } outputs[Setup::Ports::size]{
      {.note{V2MIDI::C(3)}},
      {.note{V2MIDI::Cs(3)}},
      {.note{V2MIDI::D(3)}},
      {.note{V2MIDI::Ds(3)}},
    };

    struct Pattern {
      uint16_t bpm{135};
      struct Track {
        uint8_t quarters[16];
      } tracks[Setup::Ports::size]{{64, 0, 0, 0, 0, 0, 0, 64, 0, 0, 64, 0, 0, 0, 0, 64},
                                   {0, 0, 64, 0, 0, 0, 0, 64, 64, 0, 0, 0, 0, 0, 64, 0},
                                   {0, 0, 0, 0, 64, 0, 0, 0, 64, 64, 0, 0, 0, 64, 0, 0},
                                   {0, 0, 0, 0, 0, 0, 64, 0, 0, 64, 64, 0, 64, 0, 0, 0}};
    } pattern;
  } ConfigurationDefault;

  class Device : public V2Device {
  public:
    Device() : V2Device() {
      metadata.description = "Drum Controller";
      metadata.vendor      = "Versio Duo";
      metadata.product     = "V2 beat-4";
      metadata.home        = "https://versioduo.com/#beat";
      help.device          = "4 channels intelligent USB-powered solenoid controller.";
      help.configuration   = "# Playing Notes\n"
                             "The 4 channels are configured to listen to incoming notes. The watts and seconds "
                             "values of the note velocity 1 and 127 are configured. The velocity of the incoming note "
                             "is used to calculate a pulse in the configured range.\n"
                             "A currently active pulse will end if a NoteOff is received. By configuring larger values "
                             "for seconds, this can be used to drive solenoids with the actual note length.\n"
                             "# LEDs\n"
                             "The colour of the channel LED reflect the resistance of the connected solenoid. A red "
                             "channel LED signals that the channel is short-circuit and internally disabled. The "
                             "solenoid connection should be checked.\n"
                             "A magenta-coloured flash of all  LEDs shows that the power limit has been reached, and "
                             "the device is reset. The velocity of the playing notes or the number of simultaneously "
                             "active tracks should be reduced.";
      usb.ports.standard   = 0;
      system.download      = "https://versioduo.com/download";
      system.configure     = "https://versioduo.com/configure";
      configuration        = {.version{0}, .size{sizeof(config)}, .data{&config}};
    }

    enum class CC {
      Volume = V2MIDI::CC::ChannelVolume,
    };

    Configuration config{ConfigurationDefault};

    const Configuration::Pattern getPattern() const {
      return config.pattern;
    }

    auto pulse(uint8_t port, uint8_t velocity) -> void {
      led.flash(0.02, 0.3);

      if (velocity == 0) {
        Solenoids.triggerPort(port, 0, 0, false, config.outputs[port].fadeOut);
        return;
      }

      if (_volume > 0) {
        auto fraction{(float)velocity / 127.f};
        fraction = powf(fraction, 2);
        fraction = adjustVolume(fraction);

        auto watts{config.outputs[port].min.watts};
        watts += (config.outputs[port].max.watts - config.outputs[port].min.watts) * fraction;

        auto seconds{config.outputs[port].min.seconds};
        seconds += (config.outputs[port].max.seconds - config.outputs[port].min.seconds) * fraction;
        Solenoids.triggerPort(port, watts, seconds, config.outputs[port].fadeIn, config.outputs[port].fadeOut);
      }
    }

  private:
    uint8_t _volume{100};

    auto handleReset() -> void override {
      _volume = 100;
      LED.reset();
      Solenoids.reset();
    }

    auto adjustVolume(float fraction) -> float {
      if (_volume < 100) {
        auto range{float(_volume) / 100.f};
        return fraction * range;
      }

      auto range{float(_volume - 100) / 27.f};
      return powf(fraction, 1 - (0.5f * range));
    }

    auto play(uint8_t note, uint8_t velocity) -> void {
      for (uint8_t i{}; i < Setup::Ports::size; i++) {
        if (config.outputs[i].note != note)
          continue;

        pulse(i, velocity);
      }
    }

    auto allNotesOff() -> void {
      _volume = 100;

      for (uint8_t i{}; i < Setup::Ports::size; i++)
        Solenoids.triggerPort(i, 0, 0, false, true);
    }

    auto handleNote(uint8_t channel, uint8_t note, uint8_t velocity) -> void override {
      if (channel != config.channel)
        return;

      play(note, velocity);
    }

    auto handleNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) -> void override {
      if (channel != config.channel)
        return;

      play(note, 0);
    }

    auto handleControlChange(uint8_t channel, uint8_t controller, uint8_t value) -> void override {
      if (channel != config.channel)
        return;

      switch (controller) {
        case V2MIDI::CC::AllSoundOff:
        case V2MIDI::CC::AllNotesOff:
          allNotesOff();
          break;

        case (uint8_t)CC::Volume:
          _volume = value;
          break;
      }
    }

    auto handleSystemExclusive(const uint8_t* buffer, uint32_t len) -> void override {
      if (len < 10)
        return;

      // 0x7d == SysEx prototype/research/private ID
      if (buffer[1] != 0x7d)
        return;

      // Handle only JSON messages.
      if (buffer[2] != '{' || buffer[len - 2] != '}')
        return;

      // Read incoming message.
      JsonDocument json;
      if (deserializeJson(json, buffer + 2, len - 1))
        return;

      JsonObject pulse = json["pulse"];
      if (!pulse)
        return;

      if (pulse["index"].isNull())
        return;

      uint8_t index{pulse["index"]};
      if (index > 15)
        return;

      float watts{pulse["watts"]};
      if (watts <= 0)
        return;

      float seconds{pulse["seconds"]};
      if (seconds <= 0)
        return;

      Solenoids.triggerPort(index, watts, seconds);
    }

    auto handleSystemReset() -> void override {
      reset();
    }

    auto exportLinks(JsonArray json) -> void override {
      JsonObject jsonLink{json.add<JsonObject>()};
      jsonLink["description"] = "Web Sequencer";

      std::string link{"https://versioduo.com/sequencer?connect="};
      if (!usb.name.empty())
        link.append(usb.name);
      else
        link.append(metadata.product);
      jsonLink["target"] = link;
    }

    void exportSystem(JsonObject json) override {
      auto jsonPower{json["power"].to<JsonObject>()};
      jsonPower["voltage"]       = serialized(String(Power.getVoltage(), 1));
      jsonPower["interruptions"] = Power.getInterruptions();

      if (Power.isOn())
        jsonPower["current"] = serialized(String(Solenoids.getCurrent(), 1));

      auto jsonOutputs{json["outputs"].to<JsonArray>()};
      for (uint8_t i{}; i < Setup::Ports::size; i++) {
        auto jsonOutput{jsonOutputs.add<JsonObject>()};
        jsonOutput["resistance"] = serialized(String(Solenoids.getResistance(i), 1));
      }
    }

    auto exportSettings(JsonArray json) -> void override {
      {
        auto s{json.add<JsonObject>()};
        s["type"]  = "title";
        s["title"] = "MIDI";
      }
      {
        auto s{json.add<JsonObject>()};
        s["type"]  = "number";
        s["label"] = "Channel";
        s["min"]   = 1;
        s["max"]   = 16;
        s["input"] = "select";
        s["path"]  = "midi/channel";
      }

      for (uint8_t i{}; i < Setup::Ports::size; i++) {
        {
          auto s{json.add<JsonObject>()};
          s["type"] = "title";
          char name[16];
          sprintf(name, "Output %d", i + 1);
          s["title"] = name;
        }
        {
          auto s{json.add<JsonObject>()};
          s["type"]    = "note";
          s["label"]   = "Note";
          s["default"] = ConfigurationDefault.outputs[i].note;
          char path[64];
          sprintf(path, "outputs[%d]/note", i);
          s["path"] = path;
        }
        {
          auto s{json.add<JsonObject>()};
          s["type"]  = "pulse";
          s["ruler"] = true;
          s["label"] = "Min";
          s["index"] = i;
          auto limit{s["limit"].to<JsonObject>()};
          limit["watts"] = 30;
          auto defaults{s["default"].to<JsonObject>()};
          defaults["watts"]   = serialized(String(ConfigurationDefault.outputs[i].min.watts, 1));
          defaults["seconds"] = serialized(String(ConfigurationDefault.outputs[i].min.seconds, 3));
          char path[64];
          sprintf(path, "outputs[%d]/min", i);
          s["path"] = path;
        }
        {
          auto s{json.add<JsonObject>()};
          s["type"]  = "pulse";
          s["ruler"] = true;
          s["label"] = "Max";
          s["index"] = i;
          auto limit{s["limit"].to<JsonObject>()};
          limit["watts"] = 30;
          auto defaults{s["default"].to<JsonObject>()};
          defaults["watts"]   = serialized(String(ConfigurationDefault.outputs[i].max.watts, 1));
          defaults["seconds"] = serialized(String(ConfigurationDefault.outputs[i].max.seconds, 3));
          char path[64];
          sprintf(path, "outputs[%d]/max", i);
          s["path"] = path;
        }
        {
          auto s{json.add<JsonObject>()};
          s["type"]  = "toggle";
          s["label"] = "Fade In";
          char path[64];
          sprintf(path, "outputs[%d]/fadeIn", i);
          s["path"] = path;
        }
        {
          auto s{json.add<JsonObject>()};
          s["type"]  = "toggle";
          s["label"] = "Fade Out";
          char path[64];
          sprintf(path, "outputs[%d]/fadeOut", i);
          s["path"] = path;
        }
      }

      {
        auto s{json.add<JsonObject>()};
        s["type"]     = "title";
        s["title"]    = "Pattern";
        s["subtitle"] = "Played with a long-press of the Button";
      }
      {
        auto s{json.add<JsonObject>()};
        s["type"] = "json";
        s["text"] = "Paste the clipboard from V2 sequencer";
        s["name"] = "com.versioduo.sequencer.pattern";
        s["path"] = "pattern";
      }
    }

    auto exportConfiguration(JsonObject json) -> void override {
      {
        json["#midi"] = "The MIDI settings";
        auto jsonMidi{json["midi"].to<JsonObject>()};
        jsonMidi["#channel"] = "The channel to send notes and control values to";
        jsonMidi["channel"]  = config.channel + 1;
      }

      json["#outputs"] = "The pulse parameters when playing notes";
      auto jsonOutputs{json["outputs"].to<JsonArray>()};

      for (uint8_t i{}; i < Setup::Ports::size; i++) {
        auto jsonOutput{jsonOutputs.add<JsonObject>()};

        if (i == 0)
          jsonOutput["#note"] = "The note number";
        jsonOutput["note"] = config.outputs[i].note;

        if (i == 0)
          jsonOutput["#min"] = "Mimimum (velocity 1)";
        auto jsonMin{jsonOutput["min"].to<JsonObject>()};
        jsonMin["watts"]   = serialized(String(config.outputs[i].min.watts, 1));
        jsonMin["seconds"] = serialized(String(config.outputs[i].min.seconds, 3));

        if (i == 0)
          jsonOutput["#max"] = "Maximum (velocity 127)";
        auto jsonMax{jsonOutput["max"].to<JsonObject>()};
        jsonMax["watts"]   = serialized(String(config.outputs[i].max.watts, 1));
        jsonMax["seconds"] = serialized(String(config.outputs[i].max.seconds, 3));

        if (i == 0)
          jsonOutput["#fadeIn"] = "Soft start";
        jsonOutput["fadeIn"] = config.outputs[i].fadeIn;

        if (i == 0)
          jsonOutput["#fadeOut"] = "Soft stop";
        jsonOutput["fadeOut"] = config.outputs[i].fadeOut;
      }

      json["#pattern"] = "The stored pattern to play when the button is pressed.";
      auto jsonPattern{json["pattern"].to<JsonObject>()};
      jsonPattern["bpm"] = config.pattern.bpm;

      auto jsonTracks{jsonPattern["tracks"].to<JsonArray>()};
      for (uint8_t track{}; track < Setup::Ports::size; track++) {
        auto jsonTrack{jsonTracks.add<JsonArray>()};

        for (uint8_t i{}; i < 16; i++)
          jsonTrack[i] = config.pattern.tracks[track].quarters[i];
      }
    }

    auto importConfiguration(JsonObject json) -> void override {
      JsonObject jsonMidi{json["midi"]};
      if (jsonMidi) {
        if (!jsonMidi["channel"].isNull()) {
          uint8_t channel{jsonMidi["channel"]};

          if (channel < 1)
            config.channel = 0;
          else if (channel > 16)
            config.channel = 15;
          else
            config.channel = channel - 1;
        }
      }

      JsonArray jsonOutputs = json["outputs"];
      if (jsonOutputs) {
        for (uint8_t i{}; i < Setup::Ports::size; i++) {
          JsonObject jsonOutput = jsonOutputs[i];

          if (!jsonOutput["note"].isNull()) {
            uint8_t note{jsonOutput["note"]};
            if (note > 127 - (Setup::Ports::size - 1))
              note = 127 - (Setup::Ports::size - 1);

            config.outputs[i].note = note;
          }

          JsonObject jsonMin = jsonOutput["min"];
          if (jsonMin) {
            if (!jsonMin["watts"].isNull()) {
              float watts{jsonMin["watts"]};
              if (watts < 0)
                watts = 0;
              else if (watts > 100)
                watts = 100;
              config.outputs[i].min.watts = watts;
            }

            if (!jsonMin["seconds"].isNull()) {
              float seconds{jsonMin["seconds"]};
              if (seconds < 0)
                seconds = 0;
              else if (seconds > 100)
                seconds = 100;
              config.outputs[i].min.seconds = seconds;
            }
          }

          JsonObject jsonMax = jsonOutput["max"];
          if (jsonMax) {
            if (!jsonMax["watts"].isNull()) {
              float watts{jsonMax["watts"]};
              if (watts < 0)
                watts = 0;
              else if (watts > 100)
                watts = 100;
              config.outputs[i].max.watts = watts;
            }

            if (!jsonMax["seconds"].isNull()) {
              float seconds{jsonMax["seconds"]};
              if (seconds < 0)
                seconds = 0;
              else if (seconds > 100)
                seconds = 100;
              config.outputs[i].max.seconds = seconds;
            }
          }

          if (!jsonOutput["fadeIn"].isNull())
            config.outputs[i].fadeIn = jsonOutput["fadeIn"];

          if (!jsonOutput["fadeOut"].isNull())
            config.outputs[i].fadeOut = jsonOutput["fadeOut"];
        }
      }

      JsonObject jsonPattern = json["pattern"];
      if (jsonPattern) {
        if (!jsonPattern["bpm"].isNull()) {
          uint16_t bpm{jsonPattern["bpm"]};
          if (bpm < 1)
            bpm = 1;
          else if (bpm > 1000)
            bpm = 1000;

          config.pattern.bpm = bpm;
        }

        JsonArray jsonTracks = jsonPattern["tracks"];
        if (jsonTracks) {
          for (uint8_t track{}; track < Setup::Ports::size; track++) {
            auto jsonTrack{jsonTracks[track]};
            if (!jsonTrack.isNull()) {
              for (uint8_t i{}; i < 16; i++) {
                if (!jsonTrack[i].isNull()) {
                  uint8_t beat{jsonTrack[i]};
                  if (beat < 0)
                    beat = 0;
                  else if (beat > 127)
                    beat = 127;

                  config.pattern.tracks[track].quarters[i] = beat;

                } else {
                  config.pattern.tracks[track].quarters[i] = 0;
                }
              }

            } else {
              for (uint8_t i{}; i < 16; i++)
                config.pattern.tracks[track].quarters[i] = 0;
            }
          }
        }
      }

      reset();
    }

    auto exportInput(JsonObject json) -> void override {
      json["channel"] = config.channel;

      auto jsonControllers{json["controllers"].to<JsonArray>()};
      {
        auto jsonController{jsonControllers.add<JsonObject>()};
        jsonController["name"]   = "Volume";
        jsonController["number"] = (uint8_t)CC::Volume;
        jsonController["value"]  = _volume;
      }

      auto jsonNotes{json["notes"].to<JsonArray>()};
      for (uint8_t i{}; i < Setup::Ports::size; i++) {
        auto jsonNote{jsonNotes.add<JsonObject>()};
        char name[16];
        sprintf(name, "Output %d", i + 1);
        jsonNote["name"]   = name;
        jsonNote["number"] = config.outputs[i].note;
      }
    }
  } Device;

  // Dispatch MIDI packets.
  class MIDI {
  public:
    void loop() {
      if (Device.usb.midi.receive(_midi))
        Device.dispatch(&Device.usb.midi, &_midi);

      if (MIDISerial.receive(_midi))
        Device.dispatch(&Device.usb.midi, &_midi);
    }

  private:
    V2MIDI::Packet _midi{};
  } MIDI;

  class {
  public:
    void stop() {
      _running = false;
    }

    auto loop() -> void {
      if (!_running)
        return;

      if (V2Base::getUsecSince(_usec) < _quarterUsec)
        return;

      _usec = V2Base::getUsec();

      for (uint8_t i{}; i < Setup::Ports::size; i++)
        Device.pulse(i, Device.getPattern().tracks[i].quarters[_quarter]);

      _quarter++;
      if (_quarter == 16) {
        _quarter = 0;
        light();
      }
    }

    auto play() -> void {
      _running = true;
      _usec    = 0;
      _cycle   = false;

      const float beatLengthSec = 60.f / (float)Device.getPattern().bpm;
      _quarterUsec              = beatLengthSec * 250.f * 1000.f;

      light();
    }

  private:
    auto light() -> void {
      LED.hsv({_cycle ? V2Colour::Blue : V2Colour::Orange, 1, 0.5}, Setup::Button::led);
      LED.hsv({_cycle ? V2Colour::Orange : V2Colour::Blue, 1, 0.5}, Setup::Button::led + 1);
      _cycle = !_cycle;
    }

    bool     _running{};
    uint32_t _usec{};
    uint32_t _quarterUsec{};
    uint8_t  _quarter{};
    bool     _cycle{};
  } Sequencer;

  class Button : public V2Buttons::Button {
  public:
    Button() : V2Buttons::Button(&_config, PIN_BUTTON) {}

  private:
    const V2Buttons::Config _config{.clickUsec{200 * 1000}, .holdUsec{500 * 1000}};

    auto handleHold(uint8_t count) -> void override {
      Device.reset();
      Sequencer.play();
    }

    auto handleClick(uint8_t count) -> void override {
      Sequencer.stop();
      Device.reset();
    }
  } Button;
}

auto setup() -> void {
  Serial.begin(9600);

  LED.begin();
  LED.brightnessMax(0.5);

  Power.begin();
  PWM.begin();
  for (uint8_t i{}; i < Setup::Ports::size; i++)
    V2Base::Timer::PWM::setupPin(PIN_PWM_CHANNEL + i);

  ADC.begin();
  ADC.addChannel(V2Base::Analog::ADC::getChannel(PIN_RESISTANCE_SENSE));
  ADC.addChannel(V2Base::Analog::ADC::getChannel(PIN_CURRENT_SENSE));
  ADC.addChannel(V2Base::Analog::ADC::getChannel(PIN_VOLTAGE_SENSE));

  MIDISerial.begin();
  Device.ports.push_back(&MIDISerial);

  Button.begin();
  Device.begin();
  Device.reset();
}

auto loop() -> void {
  LED.loop();
  MIDI.loop();
  Power.loop();
  V2Buttons::loop();
  Solenoids.loop();
  Device.loop();
  Sequencer.loop();

  if (Device.idle())
    Device.sleep();
}
