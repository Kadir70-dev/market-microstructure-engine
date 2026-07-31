#ifndef MME_PIPE_MQH
#define MME_PIPE_MQH

#include "mme_protocol.mqh"

// Transport-spike helper only. It contains no order, trade, or execution API.
class MmePipe {
private:
   int handle;
public:
   MmePipe() : handle(INVALID_HANDLE) {}
   ~MmePipe() { Close(); }

   bool Open(const string pipe_name) {
      Close();
      handle = FileOpen(pipe_name, FILE_READ|FILE_WRITE|FILE_BIN);
      return handle != INVALID_HANDLE;
   }

   void Close() {
      if(handle != INVALID_HANDLE) {
         FileClose(handle);
         handle = INVALID_HANDLE;
      }
   }

   bool WriteHello(const MmeHelloRecord &record) {
      if(handle == INVALID_HANDLE) return false;
      return FileWriteStruct(handle, record) == sizeof(MmeHelloRecord);
   }

   bool ReadHelloAck(MmeHelloAckRecord &record) {
      if(handle == INVALID_HANDLE) return false;
      return FileReadStruct(handle, record) == sizeof(MmeHelloAckRecord);
   }

   bool ReadHello(MmeHelloRecord &record) {
      if(handle == INVALID_HANDLE) return false;
      return FileReadStruct(handle, record) == sizeof(MmeHelloRecord);
   }

   bool WriteHelloAck(const MmeHelloAckRecord &record) {
      if(handle == INVALID_HANDLE) return false;
      if(FileWriteStruct(handle, record) != sizeof(MmeHelloAckRecord)) return false;
      FileFlush(handle);
      return true;
   }

   bool WriteMarketData(const MmeMdRecord &record) {
      if(handle == INVALID_HANDLE) return false;
      if(FileWriteStruct(handle, record) != sizeof(MmeMdRecord)) return false;
      FileFlush(handle);
      return true;
   }

   bool WriteHeartbeat(const MmeHbRecord &record) {
      if(handle == INVALID_HANDLE) return false;
      if(FileWriteStruct(handle, record) != sizeof(MmeHbRecord)) return false;
      FileFlush(handle);
      return true;
   }
};

#endif
