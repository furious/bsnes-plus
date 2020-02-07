SuperGameBoy supergameboy;

#include <nall/snes/sgb.hpp>

static void op_step_default(uint16_t pc) {
  uint8_t op  = supergameboy.read_gb(pc);
  uint8_t op1 = supergameboy.read_gb(pc + 1);
  uint8_t op2 = supergameboy.read_gb(pc + 2);
  
  printf("%04x %s\n", pc, GBCPU::disassemble(pc, op, op1, op2)());
}

static void op_read_default(uint16_t addr, uint8_t data) {
  printf("op_read  %04x => %02x\n", addr, data);
}
static void op_write_default(uint16_t addr, uint8_t data) {
  printf("op_write %04x <= %02x\n", addr, data);
}

//====================
//SuperGameBoy::Packet
//====================

const char SuperGameBoy::command_name[32][64] = {
  "PAL01",    "PAL23",    "PAL03",    "PAL12",
  "ATTR_BLK", "ATTR_LIN", "ATTR_DIV", "ATTR_CHR",
  "SOUND",    "SOU_TRN",  "PAL_SET",  "PAL_TRN",
  "ATRC_EN",  "TEST_EN",  "ICON_EN",  "DATA_SND",
  "DATA_TRN", "MLT_REQ",  "JUMP",     "CHR_TRN",
  "PCT_TRN",  "ATTR_TRN", "ATTR_SET", "MASK_EN",
  "OBJ_TRN",  "19_???",   "1A_???",   "1B_???",
  "1C_???",   "1D_???",   "1E_ROM",   "1F_???",
};

void SuperGameBoy::joyp_write(bool p15, bool p14) {
  //===============
  //joypad handling
  //===============

  if(p15 == 1 && p14 == 1) {
    if(joyp_lock == 0) {
      joyp_lock = 1;
      joyp_id = (joyp_id + 1) & mmio.mlt_req;
    }
  }

  if(p14 == 1 && p15 == 0) joyp_lock ^= 1;

  //===============
  //packet handling
  //===============

  if(p15 == 0 && p14 == 0) {
    //pulse
    pulselock = false;
    packetoffset = 0;
    bitoffset = 0;
    strobelock = true;
    packetlock = false;
    return;
  }

  if(pulselock) return;

  if(p15 == 1 && p14 == 1) {
    strobelock = false;
    return;
  }

  if(strobelock) {
    if(p15 == 1 || p14 == 1) {
      //malformed packet
      packetlock = false;
      pulselock = true;
      bitoffset = 0;
      packetoffset = 0;
    } else {
      return;
    }
  }

  //p15:1, p14:0 = 0
  //p15:0, p14:1 = 1
  bool bit = (p15 == 0);
  strobelock = true;

  if(packetlock) {
    if(p15 == 1 && p14 == 0) {
      if(packetsize < 64) packet[packetsize++] = joyp_packet;
      packetlock = false;
      pulselock = true;
    }
    return;
  }

  bitdata = (bit << 7) | (bitdata >> 1);
  if(++bitoffset < 8) return;

  bitoffset = 0;
  joyp_packet[packetoffset] = bitdata;
  if(++packetoffset < 16) return;
  packetlock = true;
}

//==================
//SuperGameBoy::Core
//==================

static uint8_t null_rom[32768];

bool SuperGameBoy::init(bool version_) {
  if(!romdata) { romdata = null_rom; romsize = 32768; }
  version = version_;

  gambatte_ = new gambatte::GB;
  gambatte_->setInputGetter(this);
  gambatte_->setDebugHandler(this);

  return true;
}

void SuperGameBoy::term() {
  if(gambatte_) {
    delete gambatte_;
    gambatte_ = 0;
  }
}

unsigned SuperGameBoy::run(uint32_t *samplebuffer, unsigned samples) {
  if((mmio.r6003 & 0x80) == 0) {
    //Gameboy is inactive
    samplebuffer[0] = 0;
    return 1;
  }

  size_t samples_ = samples;
  gambatte_->runFor(buffer, 160, samplebuffer, samples_);
  return samples_;
}

void SuperGameBoy::save() {
  gambatte_->saveSavedata();
}

void SuperGameBoy::serialize(nall::serializer &s) {
  s.integer(vram_row);
  s.array(vram);

  s.integer(mmio.r6000);
  s.integer(mmio.r6003);
  s.integer(mmio.r6004);
  s.integer(mmio.r6005);
  s.integer(mmio.r6006);
  s.integer(mmio.r6007);
  s.array(mmio.r7000);
  s.integer(mmio.r7800);
  s.integer(mmio.mlt_req);

  for(unsigned i = 0; i < 64; i++) s.array(packet[i].data);
  s.integer(packetsize);

  s.integer(joyp_id);
  s.integer(joyp_lock);
  s.integer(pulselock);
  s.integer(strobelock);
  s.integer(packetlock);
  s.array(joyp_packet.data);
  s.integer(packetoffset);
  s.integer(bitdata);
  s.integer(bitoffset);

  uint8_t *savestate = new uint8_t[256 * 1024];
  if(s.mode() == serializer::Load) {
    s.array(savestate, 256 * 1024);

    file fp;
    if(fp.open("supergameboy-state.tmp", file::mode::write)) {
      fp.write(savestate, 256 * 1024);
      fp.close();

      gambatte_->loadState("supergameboy-state.tmp");
      unlink("supergameboy-state.tmp");
    }
  } else if(s.mode() == serializer::Save) {
    gambatte_->saveState(0, 0, "supergameboy-state.tmp");

    file fp;
    if(fp.open("supergameboy-state.tmp", file::mode::read)) {
      fp.read(savestate, fp.size() < 256 * 1024 ? fp.size() : 256 * 1024);
      fp.close();
    }

    unlink("supergameboy-state.tmp");
    s.array(savestate, 256 * 1024);
  } else if(s.mode() == serializer::Size) {
    s.array(savestate, 256 * 1024);
  }
  delete[] savestate;
}

void SuperGameBoy::power() {
  gambatte_->load(gambatte::GB::FORCE_DMG);
  mmio_reset();
}

void SuperGameBoy::reset() {
  gambatte_->reset();
  mmio_reset();
}

void SuperGameBoy::row(unsigned row) {
  mmio.r7800 = 0;
  vram_row = row;
  render(vram_row);
}

uint8_t SuperGameBoy::read(uint16_t addr) {
  //LY counter
  if(addr == 0x6000) {
    return gambatte_->lyCounter();
  }

  //command ready port
  if(addr == 0x6002) {
    bool data = packetsize > 0;
    if(data) {
      for(unsigned i = 0; i < 16; i++) mmio.r7000[i] = packet[0][i];
      packetsize--;
      for(unsigned i = 0; i < packetsize; i++) packet[i] = packet[i + 1];
    }
    return data;
  }

  //command port
  if((addr & 0xfff0) == 0x7000) {
    return mmio.r7000[addr & 15];
  }

  if(addr == 0x7800) {
    uint8_t data = vram[mmio.r7800];
    mmio.r7800 = (mmio.r7800 + 1) % 320;
    return data;
  }

  return 0x00;
}

uint8_t SuperGameBoy::read_gb(uint16_t addr) {
  return gambatte_->debugRead(addr);
}

void SuperGameBoy::write(uint16_t addr, uint8_t data) {
  //control port
  //d7 = /RESET line (0 = stop, 1 = run)
  //d5..4 = multiplayer select
  if(addr == 0x6003) {
    if((mmio.r6003 & 0x80) == 0x00 && (data & 0x80) == 0x80) {
      reset();
      command_1e();
    }

    mmio.mlt_req = (data & 0x30) >> 4;
    if(mmio.mlt_req == 2) mmio.mlt_req = 3;
    joyp_id &= mmio.mlt_req;

    mmio.r6003 = data;
    return;
  }

  if(addr == 0x6004) { mmio.r6004 = data; return; }  //joypad 1 state
  if(addr == 0x6005) { mmio.r6005 = data; return; }  //joypad 2 state
  if(addr == 0x6006) { mmio.r6006 = data; return; }  //joypad 3 state
  if(addr == 0x6007) { mmio.r6007 = data; return; }  //joypad 4 state
}

void SuperGameBoy::write_gb(uint16_t addr, uint8_t data) {
  gambatte_->debugWrite(addr, data);
}

void SuperGameBoy::mmio_reset() {
  mmio.r6000 = 0x00;
  mmio.r6003 = 0x00;
  mmio.r6004 = 0xff;
  mmio.r6005 = 0xff;
  mmio.r6006 = 0xff;
  mmio.r6007 = 0xff;
  for(unsigned n = 0; n < 16; n++) mmio.r7000[n] = 0;
  mmio.r7800 = 0;
  mmio.mlt_req = 0;

  packetsize = 0;

  vram_row = 0;
  memset(vram, 0, 320);

  joyp_id = 0;
  joyp_lock = 0;
  pulselock = true;
}

//simulate 256-byte internal SGB BIOS on /RESET
void SuperGameBoy::command_1e() {
  for(unsigned i = 0; i < 6; i++) {
    Packet p;
    p[0] = 0xf1 + (i << 1);
    p[1] = 0;
    for(unsigned n = 2; n < 16; n++) {
      uint8_t data = romdata[0x0104 + (i * 14) + (n - 2)];
      p[1] += data;
      p[n] = data;
    }
    if(packetsize < 64) packet[packetsize++] = p;
  }
}

void SuperGameBoy::render(unsigned row) {
  uint32_t *source = buffer + row * 160 * 8;
  memset(vram, 0x00, 320);

  for(unsigned y = row * 8; y < row * 8 + 8; y++) {
    for(unsigned x = 0; x < 160; x++) {
      unsigned pixel = *source++ / 0x555555;
      pixel ^= 3;

      unsigned addr = (x / 8 * 16) + ((y & 7) * 2);
      vram[addr + 0] |= ((pixel & 1) >> 0) << (7 - (x & 7));
      vram[addr + 1] |= ((pixel & 2) >> 1) << (7 - (x & 7));
    }
  }
}

//==========================
//Gambatte::InputGetter
//==========================

unsigned SuperGameBoy::operator()() {
  unsigned inputState = 0x00;
  unsigned data = 0xFF;
  switch(joyp_id) {
    case 0: data = mmio.r6004; break;
    case 1: data = mmio.r6005; break;
    case 2: data = mmio.r6006; break;
    case 3: data = mmio.r6007; break;
  }
  inputState |= (joyp_id << 8);

  if (!(data & 0x80)) inputState |= gambatte::InputGetter::START;
  if (!(data & 0x40)) inputState |= gambatte::InputGetter::SELECT;
  if (!(data & 0x20)) inputState |= gambatte::InputGetter::B;
  if (!(data & 0x10)) inputState |= gambatte::InputGetter::A;
  if (!(data & 0x08)) inputState |= gambatte::InputGetter::DOWN;
  if (!(data & 0x04)) inputState |= gambatte::InputGetter::UP;
  if (!(data & 0x02)) inputState |= gambatte::InputGetter::LEFT;
  if (!(data & 0x01)) inputState |= gambatte::InputGetter::RIGHT;

  return inputState;
}

//==========================
//SuperGameBoy::Construction
//==========================

SuperGameBoy::SuperGameBoy() : gambatte_(0) {
  romdata = ramdata = rtcdata = 0;
  romsize = ramsize = rtcsize = 0;
  buffer = new uint32_t[160 * 144];
  
  op_step = op_step_default;
  op_call = op_step_default;
  op_ret  = op_step_default;
  op_irq  = op_step_default;
  op_read = op_read_default;
  op_readpc = op_read_default;
  op_write = op_write_default;
}

SuperGameBoy::~SuperGameBoy() {
  delete[] buffer;
}
