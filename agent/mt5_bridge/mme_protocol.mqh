#ifndef MME_PROTOCOL_MQH
#define MME_PROTOCOL_MQH

#define MME_PROTOCOL_MAGIC   0x31454D4D
#define MME_PROTOCOL_VERSION 1
#define MME_SYMBOL_COUNT     5

enum MmeRecordType { MME_HELLO = 1, MME_HELLO_ACK = 2, MME_MARKET_DATA = 3,
                     MME_HEARTBEAT = 4, MME_SPIKE_MESSAGE = 32767 };
enum MmeMarketDataType { MME_MD_QUOTE = 1, MME_MD_TRADE = 2 };
enum MmeBookSource { MME_L1_ONLY = 0, MME_DOM_AGGREGATED = 1,
                     MME_DOM_SYNTHETIC = 2, MME_L2_EXCHANGE = 3, MME_L3_MBO = 4 };
enum MmeMarginMode { MME_MARGIN_NETTING = 0, MME_MARGIN_EXCHANGE = 1, MME_MARGIN_HEDGING = 2 };

// MQL5 does not implement C/C++ #pragma pack. The field order below is
// deliberately alignment-neutral and mirrors the frozen one-byte-packed C++
// offsets exactly; Phase A verifies every sizeof value in MetaEditor.
struct MmeRecordPrefix {
   uint   magic;             // offset 0, width 4
   ushort version;           // offset 4, width 2
   ushort type;              // offset 6, width 2
   ulong  session_epoch;     // offset 8, width 8
   ulong  sequence;          // offset 16, width 8; total 24
};

struct MmeHelloRecord {
   MmeRecordPrefix prefix;   // offset 0, width 24
   ulong account_expected;   // offset 24, width 8
   uchar symbol_set_hash[32];// offset 32, width 32; total 64
};

struct MmeHelloAckRecord {
   MmeRecordPrefix prefix;    // offset 0, width 24
   ulong account_actual;      // offset 24, width 8
   uchar server_hash[32];     // offset 32, width 32
   uint account_margin_mode;  // offset 64, width 4
   uchar book_source[5];      // offset 68, width 5
   uchar reserved[3];         // offset 73, width 3
   uchar symbol_meta_hash[32];// offset 76, width 32; total 108
};

struct MmeMdRecord {
   MmeRecordPrefix prefix;    // offset 0, width 24
   ushort market_data_type;   // offset 24, width 2
   ushort reserved0;          // offset 26, width 2
   uint symbol_id;            // offset 28, width 4
   ulong ts_broker_ms;        // offset 32, width 8
   ulong ts_terminal_ms;      // offset 40, width 8
   long bid_ticks;            // offset 48, width 8
   long ask_ticks;            // offset 56, width 8
   long last_ticks;           // offset 64, width 8
   long bid_volume;           // offset 72, width 8
   long ask_volume;           // offset 80, width 8
   long tick_volume;          // offset 88, width 8
   uint flags;                // offset 96, width 4; total 100
};

struct MmeHbRecord {
   MmeRecordPrefix prefix;    // offset 0, width 24
   ulong ts_terminal_ms;      // offset 24, width 8
   ulong account;             // offset 32, width 8
   uchar server_hash[32];     // offset 40, width 32
   uint trade_mode;           // offset 72, width 4
   uint margin_mode;          // offset 76, width 4
   uchar connected;           // offset 80, width 1
   uchar trade_allowed;       // offset 81, width 1
   uchar book_source[5];      // offset 82, width 5
   uchar reserved0;           // offset 87, width 1
   long equity_minor;         // offset 88, width 8
   long balance_minor;        // offset 96, width 8
   long margin_minor;         // offset 104, width 8
   long free_margin_minor;    // offset 112, width 8
   uint account_currency_id;  // offset 120, width 4
   uint engine_hb_age_ms;     // offset 124, width 4; total 128
};

struct MmeSpikeMessage {
   MmeRecordPrefix prefix;   // offset 0, width 24
   ulong payload_sequence;   // offset 24, width 8
   uchar payload[88];        // offset 32, width 88
   ulong checksum;           // offset 120, width 8; total 128
};

// MQL5 and supported MT5/Wine hosts are little-endian. The C++ peer refuses
// non-little-endian hosts; no byte-swapping mode is permitted by protocol v1.

#endif
