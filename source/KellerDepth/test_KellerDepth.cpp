#include "Test.hpp"
#include "../../src/Sensors/KellerDepth/Protocol.hpp"
#include <limits>
using namespace Sensors::KellerDepth;
static std::vector<uint8_t> wire(std::vector<uint8_t> b)
{ uint16_t crc=crc16(b.data(),b.size()); b.push_back(crc>>8); b.push_back(crc&255); return b; }
static Frame sample(float value, uint8_t status=0)
{
  uint32_t word; std::memcpy(&word,&value,4);
  return Frame{wire({1,73,uint8_t(word>>24),uint8_t(word>>16),uint8_t(word>>8),uint8_t(word),status})};
}
int main()
{
  Test test("KELLER X-Line protocol and depth conversion");
  test.boolean("published init vector", request(1,48)==std::vector<uint8_t>({1,48,0x34,0}));
  test.boolean("published read vector", request(1,73)==std::vector<uint8_t>({1,73,1,0x50,0xd6}));
  test.boolean("manufacturer CRC example", request(250,48)==std::vector<uint8_t>({250,48,4,67}));
  const auto normal=sample(2.0f);
  for (size_t split=0;split<=normal.bytes.size();++split)
  {
    Parser parser(1,73); Frame out; bool got=false;
    for (size_t i=0;i<split;++i) got=parser.feed(normal.bytes[i],out)||got;
    for (size_t i=split;i<normal.bytes.size();++i) got=parser.feed(normal.bytes[i],out)||got;
    test.boolean("arbitrary byte splitting",got&&out.bytes==normal.bytes);
  }
  Parser parser(1,73); Frame out;
  for (unsigned i=0;i<20000;++i) parser.feed(uint8_t(i),out);
  test.boolean("noise memory bounded",parser.buffered()<10);
  auto bad=normal.bytes; bad.back()^=1;
  unsigned accepted=0;
  for (auto b:bad) accepted+=parser.feed(b,out);
  for (auto b:normal.bytes) accepted+=parser.feed(b,out);
  test.boolean("bad CRC rejected, resynchronizes",accepted==1&&out.bytes==normal.bytes);
  Parser wrong(2,73); accepted=0;
  for(auto b:normal.bytes) accepted+=wrong.feed(b,out);
  test.boolean("wrong address rejected",accepted==0);
  Parser fn(1,48); accepted=0;
  for(auto b:normal.bytes) accepted+=fn.feed(b,out);
  test.boolean("wrong function rejected",accepted==0);
  Parser multi(1,73); accepted=0;
  for(unsigned k=0;k<3;++k) for(auto b:normal.bytes) accepted+=multi.feed(b,out);
  test.boolean("coalesced frames",accepted==3);
  const Frame init{wire({1,48,5,21,26,1,20,0})};
  test.boolean("supported initialization",initialized(init));
  Frame other=init; other.bytes[3]=99;
  test.boolean("unknown group rejected",!initialized(other));
  other=init; other.bytes[7]=32;
  test.boolean("bad init status rejected",!initialized(other));
  Parser ex(1,73); accepted=0;
  for(auto b:wire({1,201,32})) accepted+=ex.feed(b,out);
  test.boolean("exception framing",accepted==1&&out.exception()&&out.bytes[2]==32);
  const auto value=measurement(normal,1,1000,9.80665,11000);
  test.boolean("pressure Pa and depth metres",value.valid&&value.pascal==200000&&std::abs(value.depth-10.1971621298)<1e-8);
  for(uint8_t bit:{uint8_t(0x80),uint8_t(0x40),uint8_t(0x10),uint8_t(2)})
    test.boolean("P1/compensation/power/ERR2 fault",!measurement(sample(2,bit),1,1000,9.80665,11000).valid);
  test.boolean("irrelevant P2 fault retains P1",measurement(sample(2,4),1,1000,9.80665,11000).valid);
  test.boolean("NaN rejected",!measurement(sample(std::numeric_limits<float>::quiet_NaN()),1,1000,9.80665,11000).valid);
  test.boolean("infinity rejected",!measurement(sample(std::numeric_limits<float>::infinity()),1,1000,9.80665,11000).valid);
  test.boolean("reference required",!measurement(normal,-1,1000,9.80665,11000).valid);
  test.boolean("invalid density rejected",!measurement(normal,1,0,9.80665,11000).valid);
  test.boolean("range rejected",!measurement(normal,1,1000,9.80665,1).valid);
  test.boolean("surface clips small negative depth",measurement(sample(.99f),1,1000,9.80665,11000).depth==0);
  return test.getReturnValue();
}
