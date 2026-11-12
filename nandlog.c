// The nandlog log core. Everything chip-specific is behind nandlog_chip.h, everything platform-specific is
// behind nandlog_port.h, and the host's choices are in nandlog_conf.h.


// Header Inclusions ---------------------------------------------------------------------------------------------------

#include <string.h>
#include "nandlog.h"
#include "nandlog_chip.h"
#include "nandlog_port.h"


// CRC-32 --------------------------------------------------------------------------------------------------------------

// Reflected IEEE 802.3 CRC-32 (poly 0xEDB88320, init and final XOR 0xFFFFFFFF), identical to Python's zlib.crc32. Each
// routine continues a running value as zlib.crc32(data, value) does, starting from 0. This is built with or without
// hardware, because a port's nandlog_port_crc32() may use it

#define CRC_TABLE_COUNT                             (NANDLOG_CRC_SLICE_BY_4 ? 4 : 1)

// Table k holds the CRC of each byte value followed by k zero bytes. A table in RAM is deliberately not static, since a
// compiler that saw it is never written would put it back in flash
#if NANDLOG_CRC_TABLE_IN_RAM && NANDLOG_HAS_HARDWARE
#define CRC_TABLE_STORAGE
#else
#define CRC_TABLE_STORAGE                           static const
#endif

// Keep the software CRC out of line, so that a benchmark of nandlog_crc32_software() measures the loop the log runs
#if defined(__GNUC__) || defined(__clang__)
#define CRC_NOINLINE                                __attribute__ ((__noinline__))
#else
#define CRC_NOINLINE
#endif

CRC_TABLE_STORAGE uint32_t nandlog_crc32_table[CRC_TABLE_COUNT][256] = {
   {
      0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
      0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988, 0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
      0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
      0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
      0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172, 0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
      0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
      0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
      0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924, 0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
      0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
      0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
      0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E, 0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457,
      0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
      0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB,
      0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0, 0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9,
      0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
      0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81, 0xB7BD5C3B, 0xC0BA6CAD,
      0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A, 0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683,
      0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
      0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7,
      0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC, 0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5,
      0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
      0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79,
      0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236, 0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
      0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
      0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F, 0x72076785, 0x05005713,
      0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38, 0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21,
      0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
      0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45,
      0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2, 0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB,
      0xAED16A4A, 0xD9D65ADC, 0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
      0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF,
      0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94, 0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D
   },
#if NANDLOG_CRC_SLICE_BY_4
   {
      0x00000000, 0x191B3141, 0x32366282, 0x2B2D53C3, 0x646CC504, 0x7D77F445, 0x565AA786, 0x4F4196C7,
      0xC8D98A08, 0xD1C2BB49, 0xFAEFE88A, 0xE3F4D9CB, 0xACB54F0C, 0xB5AE7E4D, 0x9E832D8E, 0x87981CCF,
      0x4AC21251, 0x53D92310, 0x78F470D3, 0x61EF4192, 0x2EAED755, 0x37B5E614, 0x1C98B5D7, 0x05838496,
      0x821B9859, 0x9B00A918, 0xB02DFADB, 0xA936CB9A, 0xE6775D5D, 0xFF6C6C1C, 0xD4413FDF, 0xCD5A0E9E,
      0x958424A2, 0x8C9F15E3, 0xA7B24620, 0xBEA97761, 0xF1E8E1A6, 0xE8F3D0E7, 0xC3DE8324, 0xDAC5B265,
      0x5D5DAEAA, 0x44469FEB, 0x6F6BCC28, 0x7670FD69, 0x39316BAE, 0x202A5AEF, 0x0B07092C, 0x121C386D,
      0xDF4636F3, 0xC65D07B2, 0xED705471, 0xF46B6530, 0xBB2AF3F7, 0xA231C2B6, 0x891C9175, 0x9007A034,
      0x179FBCFB, 0x0E848DBA, 0x25A9DE79, 0x3CB2EF38, 0x73F379FF, 0x6AE848BE, 0x41C51B7D, 0x58DE2A3C,
      0xF0794F05, 0xE9627E44, 0xC24F2D87, 0xDB541CC6, 0x94158A01, 0x8D0EBB40, 0xA623E883, 0xBF38D9C2,
      0x38A0C50D, 0x21BBF44C, 0x0A96A78F, 0x138D96CE, 0x5CCC0009, 0x45D73148, 0x6EFA628B, 0x77E153CA,
      0xBABB5D54, 0xA3A06C15, 0x888D3FD6, 0x91960E97, 0xDED79850, 0xC7CCA911, 0xECE1FAD2, 0xF5FACB93,
      0x7262D75C, 0x6B79E61D, 0x4054B5DE, 0x594F849F, 0x160E1258, 0x0F152319, 0x243870DA, 0x3D23419B,
      0x65FD6BA7, 0x7CE65AE6, 0x57CB0925, 0x4ED03864, 0x0191AEA3, 0x188A9FE2, 0x33A7CC21, 0x2ABCFD60,
      0xAD24E1AF, 0xB43FD0EE, 0x9F12832D, 0x8609B26C, 0xC94824AB, 0xD05315EA, 0xFB7E4629, 0xE2657768,
      0x2F3F79F6, 0x362448B7, 0x1D091B74, 0x04122A35, 0x4B53BCF2, 0x52488DB3, 0x7965DE70, 0x607EEF31,
      0xE7E6F3FE, 0xFEFDC2BF, 0xD5D0917C, 0xCCCBA03D, 0x838A36FA, 0x9A9107BB, 0xB1BC5478, 0xA8A76539,
      0x3B83984B, 0x2298A90A, 0x09B5FAC9, 0x10AECB88, 0x5FEF5D4F, 0x46F46C0E, 0x6DD93FCD, 0x74C20E8C,
      0xF35A1243, 0xEA412302, 0xC16C70C1, 0xD8774180, 0x9736D747, 0x8E2DE606, 0xA500B5C5, 0xBC1B8484,
      0x71418A1A, 0x685ABB5B, 0x4377E898, 0x5A6CD9D9, 0x152D4F1E, 0x0C367E5F, 0x271B2D9C, 0x3E001CDD,
      0xB9980012, 0xA0833153, 0x8BAE6290, 0x92B553D1, 0xDDF4C516, 0xC4EFF457, 0xEFC2A794, 0xF6D996D5,
      0xAE07BCE9, 0xB71C8DA8, 0x9C31DE6B, 0x852AEF2A, 0xCA6B79ED, 0xD37048AC, 0xF85D1B6F, 0xE1462A2E,
      0x66DE36E1, 0x7FC507A0, 0x54E85463, 0x4DF36522, 0x02B2F3E5, 0x1BA9C2A4, 0x30849167, 0x299FA026,
      0xE4C5AEB8, 0xFDDE9FF9, 0xD6F3CC3A, 0xCFE8FD7B, 0x80A96BBC, 0x99B25AFD, 0xB29F093E, 0xAB84387F,
      0x2C1C24B0, 0x350715F1, 0x1E2A4632, 0x07317773, 0x4870E1B4, 0x516BD0F5, 0x7A468336, 0x635DB277,
      0xCBFAD74E, 0xD2E1E60F, 0xF9CCB5CC, 0xE0D7848D, 0xAF96124A, 0xB68D230B, 0x9DA070C8, 0x84BB4189,
      0x03235D46, 0x1A386C07, 0x31153FC4, 0x280E0E85, 0x674F9842, 0x7E54A903, 0x5579FAC0, 0x4C62CB81,
      0x8138C51F, 0x9823F45E, 0xB30EA79D, 0xAA1596DC, 0xE554001B, 0xFC4F315A, 0xD7626299, 0xCE7953D8,
      0x49E14F17, 0x50FA7E56, 0x7BD72D95, 0x62CC1CD4, 0x2D8D8A13, 0x3496BB52, 0x1FBBE891, 0x06A0D9D0,
      0x5E7EF3EC, 0x4765C2AD, 0x6C48916E, 0x7553A02F, 0x3A1236E8, 0x230907A9, 0x0824546A, 0x113F652B,
      0x96A779E4, 0x8FBC48A5, 0xA4911B66, 0xBD8A2A27, 0xF2CBBCE0, 0xEBD08DA1, 0xC0FDDE62, 0xD9E6EF23,
      0x14BCE1BD, 0x0DA7D0FC, 0x268A833F, 0x3F91B27E, 0x70D024B9, 0x69CB15F8, 0x42E6463B, 0x5BFD777A,
      0xDC656BB5, 0xC57E5AF4, 0xEE530937, 0xF7483876, 0xB809AEB1, 0xA1129FF0, 0x8A3FCC33, 0x9324FD72
   },
   {
      0x00000000, 0x01C26A37, 0x0384D46E, 0x0246BE59, 0x0709A8DC, 0x06CBC2EB, 0x048D7CB2, 0x054F1685,
      0x0E1351B8, 0x0FD13B8F, 0x0D9785D6, 0x0C55EFE1, 0x091AF964, 0x08D89353, 0x0A9E2D0A, 0x0B5C473D,
      0x1C26A370, 0x1DE4C947, 0x1FA2771E, 0x1E601D29, 0x1B2F0BAC, 0x1AED619B, 0x18ABDFC2, 0x1969B5F5,
      0x1235F2C8, 0x13F798FF, 0x11B126A6, 0x10734C91, 0x153C5A14, 0x14FE3023, 0x16B88E7A, 0x177AE44D,
      0x384D46E0, 0x398F2CD7, 0x3BC9928E, 0x3A0BF8B9, 0x3F44EE3C, 0x3E86840B, 0x3CC03A52, 0x3D025065,
      0x365E1758, 0x379C7D6F, 0x35DAC336, 0x3418A901, 0x3157BF84, 0x3095D5B3, 0x32D36BEA, 0x331101DD,
      0x246BE590, 0x25A98FA7, 0x27EF31FE, 0x262D5BC9, 0x23624D4C, 0x22A0277B, 0x20E69922, 0x2124F315,
      0x2A78B428, 0x2BBADE1F, 0x29FC6046, 0x283E0A71, 0x2D711CF4, 0x2CB376C3, 0x2EF5C89A, 0x2F37A2AD,
      0x709A8DC0, 0x7158E7F7, 0x731E59AE, 0x72DC3399, 0x7793251C, 0x76514F2B, 0x7417F172, 0x75D59B45,
      0x7E89DC78, 0x7F4BB64F, 0x7D0D0816, 0x7CCF6221, 0x798074A4, 0x78421E93, 0x7A04A0CA, 0x7BC6CAFD,
      0x6CBC2EB0, 0x6D7E4487, 0x6F38FADE, 0x6EFA90E9, 0x6BB5866C, 0x6A77EC5B, 0x68315202, 0x69F33835,
      0x62AF7F08, 0x636D153F, 0x612BAB66, 0x60E9C151, 0x65A6D7D4, 0x6464BDE3, 0x662203BA, 0x67E0698D,
      0x48D7CB20, 0x4915A117, 0x4B531F4E, 0x4A917579, 0x4FDE63FC, 0x4E1C09CB, 0x4C5AB792, 0x4D98DDA5,
      0x46C49A98, 0x4706F0AF, 0x45404EF6, 0x448224C1, 0x41CD3244, 0x400F5873, 0x4249E62A, 0x438B8C1D,
      0x54F16850, 0x55330267, 0x5775BC3E, 0x56B7D609, 0x53F8C08C, 0x523AAABB, 0x507C14E2, 0x51BE7ED5,
      0x5AE239E8, 0x5B2053DF, 0x5966ED86, 0x58A487B1, 0x5DEB9134, 0x5C29FB03, 0x5E6F455A, 0x5FAD2F6D,
      0xE1351B80, 0xE0F771B7, 0xE2B1CFEE, 0xE373A5D9, 0xE63CB35C, 0xE7FED96B, 0xE5B86732, 0xE47A0D05,
      0xEF264A38, 0xEEE4200F, 0xECA29E56, 0xED60F461, 0xE82FE2E4, 0xE9ED88D3, 0xEBAB368A, 0xEA695CBD,
      0xFD13B8F0, 0xFCD1D2C7, 0xFE976C9E, 0xFF5506A9, 0xFA1A102C, 0xFBD87A1B, 0xF99EC442, 0xF85CAE75,
      0xF300E948, 0xF2C2837F, 0xF0843D26, 0xF1465711, 0xF4094194, 0xF5CB2BA3, 0xF78D95FA, 0xF64FFFCD,
      0xD9785D60, 0xD8BA3757, 0xDAFC890E, 0xDB3EE339, 0xDE71F5BC, 0xDFB39F8B, 0xDDF521D2, 0xDC374BE5,
      0xD76B0CD8, 0xD6A966EF, 0xD4EFD8B6, 0xD52DB281, 0xD062A404, 0xD1A0CE33, 0xD3E6706A, 0xD2241A5D,
      0xC55EFE10, 0xC49C9427, 0xC6DA2A7E, 0xC7184049, 0xC25756CC, 0xC3953CFB, 0xC1D382A2, 0xC011E895,
      0xCB4DAFA8, 0xCA8FC59F, 0xC8C97BC6, 0xC90B11F1, 0xCC440774, 0xCD866D43, 0xCFC0D31A, 0xCE02B92D,
      0x91AF9640, 0x906DFC77, 0x922B422E, 0x93E92819, 0x96A63E9C, 0x976454AB, 0x9522EAF2, 0x94E080C5,
      0x9FBCC7F8, 0x9E7EADCF, 0x9C381396, 0x9DFA79A1, 0x98B56F24, 0x99770513, 0x9B31BB4A, 0x9AF3D17D,
      0x8D893530, 0x8C4B5F07, 0x8E0DE15E, 0x8FCF8B69, 0x8A809DEC, 0x8B42F7DB, 0x89044982, 0x88C623B5,
      0x839A6488, 0x82580EBF, 0x801EB0E6, 0x81DCDAD1, 0x8493CC54, 0x8551A663, 0x8717183A, 0x86D5720D,
      0xA9E2D0A0, 0xA820BA97, 0xAA6604CE, 0xABA46EF9, 0xAEEB787C, 0xAF29124B, 0xAD6FAC12, 0xACADC625,
      0xA7F18118, 0xA633EB2F, 0xA4755576, 0xA5B73F41, 0xA0F829C4, 0xA13A43F3, 0xA37CFDAA, 0xA2BE979D,
      0xB5C473D0, 0xB40619E7, 0xB640A7BE, 0xB782CD89, 0xB2CDDB0C, 0xB30FB13B, 0xB1490F62, 0xB08B6555,
      0xBBD72268, 0xBA15485F, 0xB853F606, 0xB9919C31, 0xBCDE8AB4, 0xBD1CE083, 0xBF5A5EDA, 0xBE9834ED
   },
   {
      0x00000000, 0xB8BC6765, 0xAA09C88B, 0x12B5AFEE, 0x8F629757, 0x37DEF032, 0x256B5FDC, 0x9DD738B9,
      0xC5B428EF, 0x7D084F8A, 0x6FBDE064, 0xD7018701, 0x4AD6BFB8, 0xF26AD8DD, 0xE0DF7733, 0x58631056,
      0x5019579F, 0xE8A530FA, 0xFA109F14, 0x42ACF871, 0xDF7BC0C8, 0x67C7A7AD, 0x75720843, 0xCDCE6F26,
      0x95AD7F70, 0x2D111815, 0x3FA4B7FB, 0x8718D09E, 0x1ACFE827, 0xA2738F42, 0xB0C620AC, 0x087A47C9,
      0xA032AF3E, 0x188EC85B, 0x0A3B67B5, 0xB28700D0, 0x2F503869, 0x97EC5F0C, 0x8559F0E2, 0x3DE59787,
      0x658687D1, 0xDD3AE0B4, 0xCF8F4F5A, 0x7733283F, 0xEAE41086, 0x525877E3, 0x40EDD80D, 0xF851BF68,
      0xF02BF8A1, 0x48979FC4, 0x5A22302A, 0xE29E574F, 0x7F496FF6, 0xC7F50893, 0xD540A77D, 0x6DFCC018,
      0x359FD04E, 0x8D23B72B, 0x9F9618C5, 0x272A7FA0, 0xBAFD4719, 0x0241207C, 0x10F48F92, 0xA848E8F7,
      0x9B14583D, 0x23A83F58, 0x311D90B6, 0x89A1F7D3, 0x1476CF6A, 0xACCAA80F, 0xBE7F07E1, 0x06C36084,
      0x5EA070D2, 0xE61C17B7, 0xF4A9B859, 0x4C15DF3C, 0xD1C2E785, 0x697E80E0, 0x7BCB2F0E, 0xC377486B,
      0xCB0D0FA2, 0x73B168C7, 0x6104C729, 0xD9B8A04C, 0x446F98F5, 0xFCD3FF90, 0xEE66507E, 0x56DA371B,
      0x0EB9274D, 0xB6054028, 0xA4B0EFC6, 0x1C0C88A3, 0x81DBB01A, 0x3967D77F, 0x2BD27891, 0x936E1FF4,
      0x3B26F703, 0x839A9066, 0x912F3F88, 0x299358ED, 0xB4446054, 0x0CF80731, 0x1E4DA8DF, 0xA6F1CFBA,
      0xFE92DFEC, 0x462EB889, 0x549B1767, 0xEC277002, 0x71F048BB, 0xC94C2FDE, 0xDBF98030, 0x6345E755,
      0x6B3FA09C, 0xD383C7F9, 0xC1366817, 0x798A0F72, 0xE45D37CB, 0x5CE150AE, 0x4E54FF40, 0xF6E89825,
      0xAE8B8873, 0x1637EF16, 0x048240F8, 0xBC3E279D, 0x21E91F24, 0x99557841, 0x8BE0D7AF, 0x335CB0CA,
      0xED59B63B, 0x55E5D15E, 0x47507EB0, 0xFFEC19D5, 0x623B216C, 0xDA874609, 0xC832E9E7, 0x708E8E82,
      0x28ED9ED4, 0x9051F9B1, 0x82E4565F, 0x3A58313A, 0xA78F0983, 0x1F336EE6, 0x0D86C108, 0xB53AA66D,
      0xBD40E1A4, 0x05FC86C1, 0x1749292F, 0xAFF54E4A, 0x322276F3, 0x8A9E1196, 0x982BBE78, 0x2097D91D,
      0x78F4C94B, 0xC048AE2E, 0xD2FD01C0, 0x6A4166A5, 0xF7965E1C, 0x4F2A3979, 0x5D9F9697, 0xE523F1F2,
      0x4D6B1905, 0xF5D77E60, 0xE762D18E, 0x5FDEB6EB, 0xC2098E52, 0x7AB5E937, 0x680046D9, 0xD0BC21BC,
      0x88DF31EA, 0x3063568F, 0x22D6F961, 0x9A6A9E04, 0x07BDA6BD, 0xBF01C1D8, 0xADB46E36, 0x15080953,
      0x1D724E9A, 0xA5CE29FF, 0xB77B8611, 0x0FC7E174, 0x9210D9CD, 0x2AACBEA8, 0x38191146, 0x80A57623,
      0xD8C66675, 0x607A0110, 0x72CFAEFE, 0xCA73C99B, 0x57A4F122, 0xEF189647, 0xFDAD39A9, 0x45115ECC,
      0x764DEE06, 0xCEF18963, 0xDC44268D, 0x64F841E8, 0xF92F7951, 0x41931E34, 0x5326B1DA, 0xEB9AD6BF,
      0xB3F9C6E9, 0x0B45A18C, 0x19F00E62, 0xA14C6907, 0x3C9B51BE, 0x842736DB, 0x96929935, 0x2E2EFE50,
      0x2654B999, 0x9EE8DEFC, 0x8C5D7112, 0x34E11677, 0xA9362ECE, 0x118A49AB, 0x033FE645, 0xBB838120,
      0xE3E09176, 0x5B5CF613, 0x49E959FD, 0xF1553E98, 0x6C820621, 0xD43E6144, 0xC68BCEAA, 0x7E37A9CF,
      0xD67F4138, 0x6EC3265D, 0x7C7689B3, 0xC4CAEED6, 0x591DD66F, 0xE1A1B10A, 0xF3141EE4, 0x4BA87981,
      0x13CB69D7, 0xAB770EB2, 0xB9C2A15C, 0x017EC639, 0x9CA9FE80, 0x241599E5, 0x36A0360B, 0x8E1C516E,
      0x866616A7, 0x3EDA71C2, 0x2C6FDE2C, 0x94D3B949, 0x090481F0, 0xB1B8E695, 0xA30D497B, 0x1BB12E1E,
      0x43D23E48, 0xFB6E592D, 0xE9DBF6C3, 0x516791A6, 0xCCB0A91F, 0x740CCE7A, 0x66B96194, 0xDE0506F1
   }
#endif
};

CRC_NOINLINE uint32_t nandlog_crc32_software(uint32_t crc, const void *data, uint32_t length)
{
   const uint8_t *bytes = (const uint8_t*)data;
   const uint32_t *table = nandlog_crc32_table[0];
   crc = ~crc;
#if NANDLOG_CRC_SLICE_BY_4
   // Single bytes up to a word boundary, then whole words. Each byte of a word uses the table for the bytes after it
   const uint32_t *table1 = nandlog_crc32_table[1], *table2 = nandlog_crc32_table[2], *table3 = nandlog_crc32_table[3];
   for (; length && ((uintptr_t)bytes & 3); --length)
      crc = (crc >> 8) ^ table[(crc ^ *bytes++) & 0xFF];
   for (; length >= 4; length -= 4, bytes += 4)
   {
      crc ^= (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
      crc = table3[crc & 0xFF] ^ table2[(crc >> 8) & 0xFF] ^ table1[(crc >> 16) & 0xFF] ^ table[crc >> 24];
   }
#endif
   for (; length; --length)
      crc = (crc >> 8) ^ table[(crc ^ *bytes++) & 0xFF];
   return ~crc;
}

static inline uint32_t crc32_continue(uint32_t crc, const void *data, uint32_t length)
{
#if NANDLOG_PORT_CRC32
   return nandlog_port_crc32(crc, data, length);
#else
   return nandlog_crc32_software(crc, data, length);
#endif
}

uint32_t nandlog_crc32(uint32_t crc, const void *data, uint32_t length)
{
   return crc32_continue(crc, data, length);
}


#if NANDLOG_HAS_HARDWARE


// Target Requirements -------------------------------------------------------------------------------------------------

// The on-flash and on-wire structures are native integers, so they match the little-endian format only on a
// little-endian target. If the compiler does not say which it is, nandlog_init() checks at run time
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "nandlog requires a little-endian target: its on-flash and on-wire structures are native integers"
#endif

// The formats are fixed at 32-byte headers, and a compiler that padded them would write something else
typedef char nandlog_page_header_must_be_32_bytes[(sizeof(nandlog_page_header_t) == 32) ? 1 : -1];
typedef char nandlog_meta_header_must_be_32_bytes[(sizeof(nandlog_meta_header_t) == 32) ? 1 : -1];


// Static Global Variables ---------------------------------------------------------------------------------------------

#define METADATA_RING_BLOCKS                        8
#define NO_BLOCK                                    0xFFFFFFFF
#define NO_PAGE                                     0xFFFFFFFF

// Retired blocks of the log region are kept sorted in RAM, so that a position maps to a page by binary search. Past
// this many retired blocks the log still works, but asks the driver block by block
#ifndef NANDLOG_MAX_TRACKED_RETIRED_BLOCKS
#define NANDLOG_MAX_TRACKED_RETIRED_BLOCKS          128
#endif

// The most runs of passed-over positions an epoch may record before it is sealed. A run is the pages that a power cut
// tore, or an interrupted erase left, just below the write head. The runs are kept in the epoch's metadata slot after
// its details, so the room left on the page may set a lower limit than this
#ifndef NANDLOG_MAX_PASSED_OVER_RUNS
#define NANDLOG_MAX_PASSED_OVER_RUNS                1024
#endif


// A boot can add more than one run to the list, so an epoch is sealed while this many runs are still free
#define NANDLOG_PASSED_OVER_HEADROOM                4

// The fewest runs a slot must hold, so that an epoch is not sealed by its first few torn pages. nandlog_init() refuses
// a part whose page leaves less room than this after the largest details
#define NANDLOG_MIN_PASSED_OVER_RUNS                64
#if (NANDLOG_MAX_PASSED_OVER_RUNS < NANDLOG_MIN_PASSED_OVER_RUNS)
#error "NANDLOG_MAX_PASSED_OVER_RUNS must leave room for at least NANDLOG_MIN_PASSED_OVER_RUNS runs"
#endif
#if ((NANDLOG_MAX_PAGE_SIZE_BYTES - 64 - NANDLOG_MAX_EPOCH_DETAILS_BYTES) < (4 * NANDLOG_MIN_PASSED_OVER_RUNS))
#error "NANDLOG_MAX_EPOCH_DETAILS_BYTES leaves a slot no room for the list of passed-over positions"
#endif

// How many of the head search's first probes are rotated from boot to boot (see recover_write_head())
#define HEAD_SEARCH_ROTATED_PROBES                  4

// Mutation switches for test builds only. Each one, defined to 1, removes one recovery mechanism so that sim/'s
// `make ablation` can show which checks notice. All default to 0:
//   NANDLOG_MUTATE_IGNORE_PASSED_OVER   the head search ignores the positions an epoch passed over
//   NANDLOG_MUTATE_NO_ERASE_GUARD       an erase may reach into the current epoch's own blocks
//   NANDLOG_MUTATE_READ_SLOT_TWICE      a slot is judged from a second read of its page but used from the first
//   NANDLOG_MUTATE_NO_BLOCK_ENTRY_RULE  a boot trusts a head part-way into a block with nothing whole below it
//   NANDLOG_MUTATE_NAIVE_HEAD_SEARCH    the head is the first position whose header reads erased
//   NANDLOG_MUTATE_NO_TOMBSTONES        a position that cannot keep its page is left blank, not given a tombstone
#ifndef NANDLOG_MUTATE_IGNORE_PASSED_OVER
#define NANDLOG_MUTATE_IGNORE_PASSED_OVER           0
#endif
#ifndef NANDLOG_MUTATE_NO_ERASE_GUARD
#define NANDLOG_MUTATE_NO_ERASE_GUARD               0
#endif
#ifndef NANDLOG_MUTATE_READ_SLOT_TWICE
#define NANDLOG_MUTATE_READ_SLOT_TWICE              0
#endif
#ifndef NANDLOG_MUTATE_NO_BLOCK_ENTRY_RULE
#define NANDLOG_MUTATE_NO_BLOCK_ENTRY_RULE          0
#endif
#ifndef NANDLOG_MUTATE_NAIVE_HEAD_SEARCH
#define NANDLOG_MUTATE_NAIVE_HEAD_SEARCH            0
#endif
#ifndef NANDLOG_MUTATE_NO_TOMBSTONES
#define NANDLOG_MUTATE_NO_TOMBSTONES                0
#endif

// Check switch for test builds only. Each CRC the log carries forward, rather than computing it over all of its bytes,
// is compared with a full software CRC wherever it is used, and a mismatch aborts the program. Defaults to 0
#ifndef NANDLOG_CHECK_CRC
#define NANDLOG_CHECK_CRC                           0
#endif
#if NANDLOG_CHECK_CRC
#include <stdio.h>
#include <stdlib.h>
#endif

// Byte buffers laid over words to keep them word-aligned, since the copies, compares and blank checks over them go a
// word at a time
static uint32_t cache_words[(NANDLOG_MAX_PAGE_SIZE_BYTES + 3) / 4], transfer_words[(NANDLOG_MAX_PAGE_SIZE_BYTES + 3) / 4];
static uint8_t *const cache = (uint8_t*)cache_words, *const transfer_buffer = (uint8_t*)transfer_words;
static uint32_t page_size_bytes, pages_per_block, data_bytes_per_page, erase_ahead_trigger_page;
static uint32_t metadata_ring_pages, log_first_block, log_end_block, log_block_count;
static uint32_t retired_blocks[NANDLOG_MAX_TRACKED_RETIRED_BLOCKS], retired_log_blocks, bad_block_count;
static bool retired_list_complete;

// The current epoch and its write head. 'head_index' is the position the next page goes to, which is also its
// sequence number, and 'span_last_block' is the block holding position head_index - 1
static bool is_initialized, has_epoch, log_region_full, head_block_erased, head_block_passed_over;

// Set when the head search reads a page of the epoch below its own sequence number: a block under it was retired
static bool search_saw_page_ahead, head_found_short;
static uint32_t log_epoch, starting_page, head_index, current_page, span_last_block, window_blocks;
static uint32_t metadata_ring_page, next_ring_slot, next_slot_sequence, head_failed_page, verified_blank_page, epoch_floor;

// During boot, the current epoch's passed-over positions as sorted runs, in the image of its slot loaded into the
// cache. 'passed_over_from' is where the run directly below the head begins
static uint32_t passed_over_count, passed_over_capacity, passed_over_from;
static uint8_t *passed_over_list;

// The page being assembled in RAM. 'cache_crc' covers its 'cache_index' bytes and is updated as each record is added,
// so that a commit has only its header's CRC to compute
static uint32_t cache_index, cache_crc, page_record_count, page_first_timestamp, page_last_timestamp;

// Session, write and read state. 'disabled' is set by the caller, 'unwritable' when the part stops retaining pages
// (cleared by a reboot or a new epoch), and 'sealed' when the epoch can take no more pages (cleared by a new epoch)
static bool in_session, disabled, unwritable, sealed, list_lost, layout_unverified, is_reading, epoch_is_current = true;
static uint32_t selected_epoch, selected_start_page, selected_positions, selected_ring_page;
static uint32_t reading_index, reading_end, reading_page, reading_seq;

static uint32_t retransmit_seqs[NANDLOG_MAX_RETRANSMIT_PAGES], retransmit_num_pages;
static nandlog_stats_t stats;


// Private Helper Functions --------------------------------------------------------------------------------------------

static void part_awake(void)
{
   // Wake the storage peripheral for one operation, unless a session is already holding it awake
   if (!in_session)
   {
      nandlog_port_power(true);
      nandlog_chip_low_power(false);
   }
}

static void part_asleep(void)
{
   if (!in_session)
   {
      nandlog_chip_low_power(true);
      nandlog_port_power(false);
   }
}

static inline uint32_t crc32_compute(const void *data, uint32_t length)
{
   // The CRC of these bytes alone
   return crc32_continue(0, data, length);
}

static inline void check_crc(const char *what, uint32_t carried, const void *data, uint32_t length)
{
   // Under NANDLOG_CHECK_CRC, abort if a carried-forward CRC differs from a full software CRC of the same bytes
#if NANDLOG_CHECK_CRC
   const uint32_t whole = nandlog_crc32_software(0, data, length);
   if (carried != whole)
   {
      fprintf(stderr, "NANDLOG_CHECK_CRC: %s carries CRC %08X over %u bytes, whose CRC is %08X\n", what, (unsigned)carried,
              (unsigned)length, (unsigned)whole);
      abort();
   }
#else
   (void)what;
   (void)carried;
   (void)data;
   (void)length;
#endif
}

static bool bytes_equal(const uint8_t *a, const uint8_t *b, uint32_t length)
{
   // Every committed page is read back and compared, so this compares 16 bytes a step and decides only at the end.
   // Mismatches are rare enough that an early exit is not worth a branch in every step
   uint32_t differ = 0;
   for (; length >= 16; a += 16, b += 16, length -= 16)
   {
      uint32_t x[4], y[4];
      memcpy(x, a, sizeof(x));
      memcpy(y, b, sizeof(y));
      differ |= (x[0] ^ y[0]) | (x[1] ^ y[1]) | (x[2] ^ y[2]) | (x[3] ^ y[3]);
   }
   for (; length; ++a, ++b, --length)
      differ |= (uint32_t)(*a ^ *b);
   return !differ;
}

static bool page_header_valid(const nandlog_page_header_t *header)
{
   // A page is trustworthy only if it carries the magic and its header checksums correctly
   return ((header->magic == NANDLOG_PAGE_MAGIC) || (header->magic == NANDLOG_PAGE_MAGIC_FRAMED)) &&
          (header->header_crc == crc32_compute(header, NANDLOG_PAGE_HEADER_CRC_BYTES)) &&
          (header->payload_length <= data_bytes_per_page);
}

static bool resolve_geometry(void)
{
   // The chip is the only thing that knows its own shape, so the log asks rather than assumes
   const nandlog_geometry_t *geometry = nandlog_chip_geometry();
   page_size_bytes = geometry->page_size_bytes;
   pages_per_block = geometry->pages_per_block;
   data_bytes_per_page = page_size_bytes - sizeof(nandlog_page_header_t);
   metadata_ring_pages = METADATA_RING_BLOCKS * pages_per_block;
   log_first_block = METADATA_RING_BLOCKS;
   log_end_block = geometry->block_count - geometry->reserved_blocks;
   log_block_count = log_end_block - log_first_block;
   erase_ahead_trigger_page = pages_per_block / 2;

   // A slot's list of passed-over positions shares its page with the largest details a slot may carry
   const uint32_t slot_bytes = sizeof(nandlog_meta_header_t) + sizeof(nandlog_meta_extension_t) + NANDLOG_MAX_EPOCH_DETAILS_BYTES;
   passed_over_capacity = (page_size_bytes > slot_bytes) ? ((page_size_bytes - slot_bytes) / sizeof(uint32_t)) : 0;
   if (passed_over_capacity > NANDLOG_MAX_PASSED_OVER_RUNS)
      passed_over_capacity = NANDLOG_MAX_PASSED_OVER_RUNS;
   if (passed_over_capacity >= NANDLOG_MIN_PASSED_OVER_RUNS)
      return true;
   NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: A %u-byte page leaves room for %u passed-over runs after %u bytes of details; at least %u are needed\n",
               page_size_bytes, passed_over_capacity, (uint32_t)NANDLOG_MAX_EPOCH_DETAILS_BYTES, (uint32_t)NANDLOG_MIN_PASSED_OVER_RUNS);
   return false;
}


// Blocks and Positions ------------------------------------------------------------------------------------------------

// The log region is a circle of blocks. An epoch's good positions are the pages of its blocks that are not retired,
// numbered from zero forward from the block it began at, and a page's position is also its sequence number. The driver
// persists which blocks are retired, so the positions are the same after a reboot

static inline uint32_t block_of(uint32_t page)       { return page / pages_per_block; }
static inline uint32_t page_offset(uint32_t page)    { return page & (pages_per_block - 1); }
static inline uint32_t first_page_of(uint32_t block) { return block * pages_per_block; }
static inline bool in_log_region(uint32_t block)     { return (block >= log_first_block) && (block < log_end_block); }

static inline uint32_t log_block_after(uint32_t block)
{
   // The block after 'block' in log order, wrapping from the end of the region back to its start
   return ((block + 1) < log_end_block) ? (block + 1) : log_first_block;
}

static inline uint32_t log_block_distance(uint32_t from, uint32_t to)
{
   // Blocks from one to the other going forward in log order, retired ones included
   return (to >= from) ? (to - from) : (log_block_count - (from - to));
}

static uint32_t retired_lower_bound(uint32_t block)
{
   // Index of the first tracked retired block at or after 'block'
   uint32_t low = 0, high = retired_log_blocks;
   while (low < high)
   {
      const uint32_t mid = low + ((high - low) / 2);
      if (retired_blocks[mid] < block)
         low = mid + 1;
      else
         high = mid;
   }
   return low;
}

static bool block_retired(uint32_t block)
{
   // Blocks of the log region are answered from RAM while the list is complete, and anything else asks the driver
   if (retired_list_complete && in_log_region(block))
   {
      const uint32_t i = retired_lower_bound(block);
      return (i < retired_log_blocks) && (retired_blocks[i] == block);
   }
   return nandlog_chip_is_bad_block(first_page_of(block));
}

static void track_retired_block(uint32_t block)
{
   // Keep RAM in step with a retirement the driver has just made persistent
   ++bad_block_count;
   if (!in_log_region(block))
      return;
   if (retired_list_complete && (retired_log_blocks < NANDLOG_MAX_TRACKED_RETIRED_BLOCKS))
   {
      const uint32_t at = retired_lower_bound(block);
      memmove(&retired_blocks[at + 1], &retired_blocks[at], (retired_log_blocks - at) * sizeof(retired_blocks[0]));
      retired_blocks[at] = block;
   }
   else
      retired_list_complete = false;
   ++retired_log_blocks;
}

static void build_retired_list(void)
{
   // One pass over the driver's table at boot, counting retired blocks across everything the log manages
   bad_block_count = retired_log_blocks = 0;
   retired_list_complete = true;
   for (uint32_t block = 0; block < log_end_block; ++block)
      if (nandlog_chip_is_bad_block(first_page_of(block)))
      {
         ++bad_block_count;
         if (!in_log_region(block))
            continue;
         if (retired_log_blocks < NANDLOG_MAX_TRACKED_RETIRED_BLOCKS)
            retired_blocks[retired_log_blocks] = block;
         else
            retired_list_complete = false;
         ++retired_log_blocks;
      }
}

static uint32_t good_positions(void)
{
   // Every position the region offers, which is the most an epoch can hold
   return (log_block_count - retired_log_blocks) * pages_per_block;
}

static uint32_t next_good_block(uint32_t block)
{
   // The next block in log order that is not retired, or NO_BLOCK if every other block is
   for (uint32_t steps = 1; steps < log_block_count; ++steps)
   {
      block = log_block_after(block);
      if (!block_retired(block))
         return block;
   }
   return NO_BLOCK;
}

static uint32_t good_block_at(uint32_t start_block, uint32_t index)
{
   // The index-th block that is not retired, counting from zero, going forward from 'start_block'
   if (!retired_list_complete)
   {
      uint32_t block = start_block;
      for (uint32_t steps = 0; steps < log_block_count; ++steps, block = log_block_after(block))
         if (!nandlog_chip_is_bad_block(first_page_of(block)) && !index--)
            return block;
      return NO_BLOCK;
   }

   // Each retired block at or before the candidate pushes it one block further. The sorted list, rotated to begin at
   // 'start_block', visits them in order of distance, so one pass settles it
   uint32_t candidate = index;
   const uint32_t first = retired_lower_bound(start_block);
   for (uint32_t i = 0; i < retired_log_blocks; ++i)
   {
      if (log_block_distance(start_block, retired_blocks[(first + i) % retired_log_blocks]) > candidate)
         break;
      ++candidate;
   }
   return (candidate < log_block_count) ? (log_first_block + (((start_block - log_first_block) + candidate) % log_block_count)) : NO_BLOCK;
}

static uint32_t page_at_position(uint32_t start_page, uint32_t position)
{
   // The page holding good position 'position' of an epoch begun at 'start_page'
   const uint32_t block = good_block_at(block_of(start_page), position / pages_per_block);
   return (block == NO_BLOCK) ? NO_PAGE : (first_page_of(block) + (position % pages_per_block));
}

static uint32_t position_of_page(uint32_t start_page, uint32_t page)
{
   // The inverse: how many good positions come before 'page', going forward from 'start_page'
   const uint32_t start_block = block_of(start_page), distance = log_block_distance(start_block, block_of(page));
   uint32_t retired = 0;
   if (retired_list_complete)
   {
      for (uint32_t i = 0; i < retired_log_blocks; ++i)
         if (log_block_distance(start_block, retired_blocks[i]) < distance)
            ++retired;
   }
   else
   {
      uint32_t block = start_block;
      for (uint32_t steps = 0; steps < distance; ++steps, block = log_block_after(block))
         retired += nandlog_chip_is_bad_block(first_page_of(block)) ? 1 : 0;
   }
   return ((distance - retired) * pages_per_block) + page_offset(page);
}

static uint32_t next_good_page(uint32_t page)
{
   // The page after 'page' in log order, stepping over retired blocks
   if (page_offset(page + 1))
      return page + 1;
   const uint32_t block = next_good_block(block_of(page));
   return (block == NO_BLOCK) ? NO_PAGE : first_page_of(block);
}

static bool in_epoch_span(uint32_t block)
{
   // Whether 'block' lies among the blocks holding the current epoch's pages. They run without a break from the
   // epoch's first block to the block of its newest page, so one comparison of distances answers it
   if (!has_epoch || !head_index)
      return false;
   const uint32_t start_block = block_of(starting_page);
   return log_block_distance(start_block, block) <= log_block_distance(start_block, span_last_block);
}

static bool rewrite_current_slot(uint32_t failed_page);
static void begin_slot_image(uint32_t epoch, uint32_t start_page, const void *details, uint16_t length);

// A block the log is retiring, and the block a relocation is copying into: neither may be given to the bad-block table
static uint32_t block_being_retired = NO_BLOCK, relocation_target = NO_BLOCK;
static bool give_blocks_to_the_table(void);

static bool retire_block(uint32_t block)
{
   // Make the retirement persistent before anything acts on it, since positions are laid out over the blocks the driver
   // will call good after a reboot. If the driver has nowhere to record it, give its table blocks and try again
   bool retired = nandlog_chip_mark_bad_block(first_page_of(block)) && nandlog_chip_is_bad_block(first_page_of(block));
   if (!retired && !unwritable)
   {
      block_being_retired = block;
      retired = give_blocks_to_the_table() && nandlog_chip_mark_bad_block(first_page_of(block)) && nandlog_chip_is_bad_block(first_page_of(block));
      block_being_retired = NO_BLOCK;
   }
   if (retired)
   {
      track_retired_block(block);
      ++stats.blocks_retired;
      NANDLOG_LOG(NANDLOG_LOG_WARNINGS, "WARNING: Block %u retired\n", block);

      // Record the retirement in the ring before anything is laid out past this block, so that a boot can tell if the
      // driver's table later loses it
      if (has_epoch && !rewrite_current_slot(head_failed_page))
         NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: The metadata ring could not record the retirement of block %u\n", block);
      return true;
   }

   // The retirement could not be recorded. Going on would lay out positions differently from the next boot, so the log
   // stops taking pages
   unwritable = true;
   NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: Block %u could not be retired persistently; logging disabled\n", block);
   nandlog_port_unwritable();
   return false;
}

static inline bool erase_refused(uint32_t block)
{
   // The erase guard: no block holding a position below the write head is erased
   return !NANDLOG_MUTATE_NO_ERASE_GUARD && in_epoch_span(block);
}

static bool erase_log_block(uint32_t block)
{
   // Every erase in the log region except rewrite_passed_over_block()'s comes through here, so this guard keeps the
   // current epoch's pages from being erased. A block that will not erase is retired, and false says to try another
   if ((block == NO_BLOCK) || erase_refused(block) || block_retired(block))
      return false;
   if (nandlog_chip_erase_block(first_page_of(block)))
      return true;
   retire_block(block);
   return false;
}


// Write-Head Recovery -------------------------------------------------------------------------------------------------

// A boot finds the current epoch's write head h by binary search over its good positions. At every point where power
// can fail, the write paths keep three rules:
//   (I1) Each position below h is passed over or not blank, and any valid header on it is this epoch's, carrying the
//        position as its sequence number.
//   (I2) Position h is blank and not passed over, unless it begins a block the epoch has not entered yet.
//   (I3) No position at or above h is passed over or holds this epoch's valid header for that position.
// A position is INSIDE if it is passed over or holds this epoch's page for it, OUTSIDE if it is blank or holds any
// other valid header, and otherwise UNKNOWN, taking the class of the nearest known position below it. The rules make
// INSIDE exactly "below h". Only a blank page that is not passed over counts as past the head, so unreadable pages,
// retired blocks and torn programs below the head do not mislead the search. A block the epoch has not entered may
// hold the remains of an interrupted erase, which examine_below_head() deals with.
//
// A page torn by a power cut may later read blank, which would break (I1). So a boot lists the torn pages directly
// below the head as passed over in the epoch's slot, and writes the slot before it commits anything after them

typedef enum
{
   POSITION_INSIDE,
   POSITION_OUTSIDE,
   POSITION_UNKNOWN
} position_class_t;

// The passed-over list holds sorted, non-overlapping runs of positions, one word each: the first position in the low
// 24 bits and the run's length less one in the high 8

#define RUN_FIRST(run)                              ((run) & 0x00FFFFFF)
#define RUN_LENGTH(run)                             (((run) >> 24) + 1)
#define RUN_MAX_LENGTH                              256

static uint32_t passed_over_at(uint32_t index)
{
   uint32_t run;
   memcpy(&run, passed_over_list + (index * sizeof(run)), sizeof(run));
   return run;
}

static void passed_over_put(uint32_t index, uint32_t first, uint32_t length)
{
   const uint32_t run = first | ((length - 1) << 24);
   memcpy(passed_over_list + (index * sizeof(run)), &run, sizeof(run));
}

static uint32_t passed_over_upper_bound(uint32_t position)
{
   // Index of the first run that begins after 'position'
   uint32_t low = 0, high = passed_over_count;
   while (low < high)
   {
      const uint32_t mid = low + ((high - low) / 2);
      if (RUN_FIRST(passed_over_at(mid)) <= position)
         low = mid + 1;
      else
         high = mid;
   }
   return low;
}

static bool position_passed_over(uint32_t position)
{
   const uint32_t i = passed_over_upper_bound(position);
   return i && ((position - RUN_FIRST(passed_over_at(i - 1))) < RUN_LENGTH(passed_over_at(i - 1)));
}

static bool pass_over(uint32_t position, bool *added)
{
   // Add a position to the list, extending the run before it where possible. Returns false if the list is full, and
   // sets '*added' if the list changed
   if (position_passed_over(position))
      return true;
   *added = true;
   const uint32_t i = passed_over_upper_bound(position);
   if (i)
   {
      const uint32_t before = passed_over_at(i - 1);
      if (((RUN_FIRST(before) + RUN_LENGTH(before)) == position) && (RUN_LENGTH(before) < RUN_MAX_LENGTH))
      {
         passed_over_put(i - 1, RUN_FIRST(before), RUN_LENGTH(before) + 1);
         return true;
      }
   }
   if (passed_over_count >= passed_over_capacity)
      return false;
   memmove(passed_over_list + ((i + 1) * sizeof(uint32_t)), passed_over_list + (i * sizeof(uint32_t)), (passed_over_count - i) * sizeof(uint32_t));
   passed_over_put(i, position, 1);
   ++passed_over_count;
   return true;
}

static position_class_t page_class(uint32_t epoch, uint32_t start_page, uint32_t position)
{
   // What the page at good position 'position' says about where the write head is, from the page alone
   const uint32_t page = page_at_position(start_page, position);
   if (page == NO_PAGE)
      return POSITION_OUTSIDE;
   nandlog_page_header_t header;
   const nandlog_read_result_t read = nandlog_chip_read_page_region((uint8_t*)&header, page, 0, sizeof(header));

   // A header that checksums is trusted even when the part could not correct the rest of the page
   if (page_header_valid(&header))
   {
      search_saw_page_ahead = search_saw_page_ahead || ((header.epoch == epoch) && (header.seq > position));
      return ((header.epoch == epoch) && (header.seq == position)) ? POSITION_INSIDE : POSITION_OUTSIDE;
   }

   // Only a page with no programmed bit anywhere is evidence of being past the head. An erased-looking header is not
   // enough, because a torn program or the part's ECC can show a programmed page's header as erased
   if ((read != NANDLOG_READ_UNCORRECTABLE) && nandlog_bytes_erased((const uint8_t*)&header, sizeof(header)) && nandlog_chip_page_is_blank(page))
   {
      verified_blank_page = page;
      return POSITION_OUTSIDE;
   }
   return POSITION_UNKNOWN;
}

static position_class_t classify_position(uint32_t epoch, uint32_t start_page, uint32_t position)
{
   // A position the epoch passed over is below the head whatever its page reads now
   return (!NANDLOG_MUTATE_IGNORE_PASSED_OVER && position_passed_over(position)) ? POSITION_INSIDE : page_class(epoch, start_page, position);
}

static uint32_t inside_after(uint32_t epoch, uint32_t start_page, uint32_t candidate)
{
   // The first position within a block's worth after 'candidate' that is passed over or holds this epoch's page for
   // it, or NO_PAGE if a blank-looking header or another epoch's page comes first. If the slot's list was lost, its
   // positions may read blank anywhere below the head, so blank-looking headers are looked past too
   for (uint32_t position = candidate + 1, steps = 0; (position < good_positions()) && (steps < pages_per_block); ++position, ++steps)
   {
      if (!NANDLOG_MUTATE_IGNORE_PASSED_OVER && position_passed_over(position))
         return position;
      const uint32_t page = page_at_position(start_page, position);
      nandlog_page_header_t header;
      if (page == NO_PAGE)
         break;
      nandlog_chip_read_page_region((uint8_t*)&header, page, 0, sizeof(header));
      if (page_header_valid(&header))
      {
         search_saw_page_ahead = search_saw_page_ahead || ((header.epoch == epoch) && (header.seq > position));
         return ((header.epoch == epoch) && (header.seq == position)) ? position : NO_PAGE;
      }
      if (nandlog_bytes_erased((const uint8_t*)&header, sizeof(header)) && !list_lost)
         break;
   }
   return NO_PAGE;
}

static uint32_t recover_write_head(uint32_t epoch, uint32_t start_page)
{
#if NANDLOG_MUTATE_NAIVE_HEAD_SEARCH
   for (uint32_t position = 0; position < good_positions(); ++position)
   {
      nandlog_page_header_t header;
      nandlog_chip_read_page_region((uint8_t*)&header, page_at_position(start_page, position), 0, sizeof(header));
      if (nandlog_bytes_erased((const uint8_t*)&header, sizeof(header)))
         return position;
   }
   return good_positions();
#endif
   // Positions below 'low' are INSIDE and those at or above 'high' are OUTSIDE. The first few probes vary from boot to
   // boot within the middle half of the range, so that a device that boots often does not wear the same pages by read
   // disturb
   uint32_t low = 0, high = good_positions(), rotated = HEAD_SEARCH_ROTATED_PROBES;
   uint32_t salt = crc32_compute(&next_slot_sequence, sizeof(next_slot_sequence)) ^ (uint32_t)(NANDLOG_BOOT_SALT);
   for (;;)
   {
      while (low < high)
      {
         uint32_t mid = low + ((high - low) / 2);
         if (rotated && ((high - low) >= 4))
         {
            mid = low + ((high - low) / 4) + (((salt & 0xFF) * ((high - low) / 2)) >> 8);
            salt >>= 8;
            --rotated;
         }
         position_class_t verdict = classify_position(epoch, start_page, mid);
         if (verdict == POSITION_UNKNOWN)
         {
            // Take the class of the nearest known position below it, which is INSIDE if there is none above 'low'
            uint32_t below = mid;
            verdict = POSITION_INSIDE;
            while (below > low)
            {
               const position_class_t found = classify_position(epoch, start_page, --below);
               if (found != POSITION_UNKNOWN)
               {
                  verdict = found;
                  break;
               }
            }

            // Everything from an OUTSIDE position up shares its class, the UNKNOWN run above it included
            if (verdict == POSITION_OUTSIDE)
            {
               high = below;
               continue;
            }
         }
         if (verdict == POSITION_INSIDE)
            low = mid + 1;
         else
            high = mid;
      }

      // By (I3), a page of the epoch at its own position just after the answer means the answer is a committed page
      // that now reads blank. Search again above it, and leave that page as a gap
      const uint32_t inside = inside_after(epoch, start_page, low);
      if (inside == NO_PAGE)
         return low;
      NANDLOG_LOG(NANDLOG_LOG_WARNINGS, "WARNING: Position %u of epoch %u reads blank below the write head\n", low, epoch);
      low = inside + 1;
      high = good_positions();
   }
}

static bool position_committed(uint32_t position, bool *own)
{
   // Whether the page at 'position' reads whole: it carries the epoch's header for that position ('*own'), the part
   // can correct it, and its payload CRC holds. A torn program can leave a valid header over a bad payload
   const nandlog_read_result_t read = nandlog_chip_read_page(transfer_buffer, page_at_position(starting_page, position));
   nandlog_page_header_t header;
   memcpy(&header, transfer_buffer, sizeof(header));
   *own = page_header_valid(&header) && (header.epoch == log_epoch) && (header.seq == position);
   return *own && (read != NANDLOG_READ_UNCORRECTABLE) &&
          (header.payload_crc == crc32_compute(transfer_buffer + sizeof(header), header.payload_length));
}

static uint32_t examine_below_head(uint32_t head, uint32_t *run_start, bool *passed_over_block)
{
   // Pass over the positions directly below the head that hold nothing whole of the epoch, whatever the search made of
   // them. Their sequence numbers stay consumed and nothing programs them again. If that run covers all of the head's
   // block below the head, the block may hold the remains of an interrupted erase and is not trusted. If none of it is
   // listed or carries the epoch's header, and a page past the head is not blank, the block was never entered and the
   // head goes back to its start. Otherwise the block is erased again and its passed-over positions get tombstones
   const uint32_t block_start = (head / pages_per_block) * pages_per_block;
   uint32_t start = head, entered = 0;
   while (start)
   {
      const uint32_t position = start - 1;
      bool own = false;
      if (position_committed(position, &own))
         break;
      entered += ((own || position_passed_over(position)) && (position >= block_start)) ? 1 : 0;
      --start;
   }
   *run_start = start;
   *passed_over_block = false;
   if (NANDLOG_MUTATE_NO_BLOCK_ENTRY_RULE || (head == block_start) || (start > block_start) || (head >= good_positions()))
      return head;
   if (!entered)
      for (uint32_t position = head; position < (block_start + pages_per_block); ++position)
         if (!nandlog_chip_page_is_blank(page_at_position(starting_page, position)))
            return block_start;
   *passed_over_block = true;
   return head;
}

static bool relocate_head_block(uint32_t *blocks_left);
static bool write_tombstone(uint32_t page, uint32_t seq);
static void record_head_failure(uint32_t page);
static bool move_head_past_retired_block(void)
{
   // The head's block was retired before the epoch put anything in it, so the head's position now falls at the
   // start of the next good block
   const uint32_t block = next_good_block(block_of(current_page));
   if ((block == NO_BLOCK) || in_epoch_span(block) || (head_index >= good_positions()))
   {
      log_region_full = true;
      return false;
   }
   current_page = first_page_of(block);
   head_block_erased = false;
   return true;
}

static void establish_window(void)
{
   // Erase the head's block if the epoch has nothing in it yet, then the NANDLOG_ERASE_AHEAD_BLOCKS good blocks after
   // it. This runs at boot and for each new epoch whatever the window was, since an interrupted erase can leave a block
   // that reads erased but programs unreliably
   window_blocks = 0;
   for (uint32_t tries = 0; !head_block_erased && !log_region_full && !unwritable && (tries < NANDLOG_PAGE_PLACEMENT_ATTEMPTS); ++tries)
   {
      if (erase_log_block(block_of(current_page)))
         head_block_erased = true;
      else if (!unwritable && !move_head_past_retired_block())
         return;
   }
   if (!head_block_erased || log_region_full)
      return;

   uint32_t block = block_of(current_page);
   for (uint32_t tries = 0; (window_blocks < NANDLOG_ERASE_AHEAD_BLOCKS) && !unwritable && (tries < (NANDLOG_ERASE_AHEAD_BLOCKS + NANDLOG_PAGE_PLACEMENT_ATTEMPTS)); ++tries)
   {
      const uint32_t next = next_good_block(block);
      if ((next == NO_BLOCK) || (next == block_of(current_page)) || erase_refused(next))
         break;
      if (erase_log_block(next))
      {
         ++window_blocks;
         block = next;
      }
   }
}

static void locate_write_head(void)
{
   // Find the write head of the current epoch, reading only
   verified_blank_page = NO_PAGE;
   search_saw_page_ahead = false;
   const uint32_t found = recover_write_head(log_epoch, starting_page);
   head_found_short = search_saw_page_ahead;
   head_index = examine_below_head(found, &passed_over_from, &head_block_passed_over);
   stats.positions_skipped = head_index - passed_over_from;
   NANDLOG_LOG(NANDLOG_LOG_INFO, "INFO: Epoch %u write head recovered at position %u (search found %u; %u position(s) passed over below it)\n",
               log_epoch, head_index, found, head_index - passed_over_from);
   span_last_block = head_index ? block_of(page_at_position(starting_page, head_index - 1)) : NO_BLOCK;
   head_block_erased = false;
   window_blocks = 0;
   log_region_full = (head_index >= good_positions());
   current_page = log_region_full ? NO_PAGE : page_at_position(starting_page, head_index);
}

static uint32_t good_block_before(uint32_t block, uint32_t count)
{
   // The block 'count' good blocks before 'block' in log order, or NO_BLOCK
   for (uint32_t steps = 0; count && (steps < log_block_count); ++steps)
   {
      block = (block > log_first_block) ? (block - 1) : (log_end_block - 1);
      if (!block_retired(block))
         --count;
   }
   return count ? NO_BLOCK : block;
}

static bool adopt_lost_epochs(bool *adopted)
{
   // An epoch begins only once its slot is on the part twice, so a page of an epoch newer than the ring names means
   // every copy of that epoch's slot has become unreadable. Look for one where a newer epoch would begin and in the
   // window after it. Any such page fixes where its epoch began, and the boot continues that epoch without its details.
   // False means pages of a newer epoch that cannot be placed, and then nothing may be written
   *adopted = false;
   for (uint32_t steps = 0; steps < (METADATA_RING_BLOCKS * pages_per_block); ++steps)
   {
      uint32_t block = log_region_full ? good_block_at(block_of(starting_page), 0) :
                       (!page_offset(current_page) ? block_of(current_page) : next_good_block(block_of(current_page)));
      nandlog_page_header_t header;
      bool newer = false;
      uint32_t offset = 0;
      for (uint32_t k = 0; !newer && (block != NO_BLOCK) && (k < (NANDLOG_ERASE_AHEAD_BLOCKS + 2)); ++k)
      {
         for (offset = 0; offset < pages_per_block; ++offset)
         {
            nandlog_chip_read_page_region((uint8_t*)&header, first_page_of(block) + offset, 0, sizeof(header));
            if (page_header_valid(&header) || nandlog_bytes_erased((const uint8_t*)&header, sizeof(header)))
               break;
         }
         newer = (offset < pages_per_block) && page_header_valid(&header) && (header.epoch > log_epoch);
         if (!newer)
            block = next_good_block(block);
      }
      if (!newer)
         return true;
      const uint32_t start = (header.seq >= offset) ? good_block_before(block, (header.seq - offset) / pages_per_block) : NO_BLOCK;
      if ((start == NO_BLOCK) || ((header.seq - offset) % pages_per_block))
      {
         NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: Pages of epoch %u follow epoch %u, which the metadata ring names as current, and cannot be placed; logging disabled\n",
                     header.epoch, log_epoch);
         epoch_floor = header.epoch;
         return false;
      }
      NANDLOG_LOG(NANDLOG_LOG_WARNINGS, "WARNING: Epoch %u has pages but no readable metadata slot; continuing it\n", header.epoch);
      log_epoch = header.epoch;
      starting_page = first_page_of(start);
      head_failed_page = NO_PAGE;
      passed_over_count = 0;
      list_lost = true;
      begin_slot_image(log_epoch, starting_page, NULL, 0);
      *adopted = true;
      locate_write_head();
   }
   return true;
}

static bool write_boot_slot(uint8_t sealed);
static bool relocate_at_boot(void)
{
   // Move the head's block out of the way before the epoch programs anything. The ring first records it as a failed
   // program at the head, so that a boot that finds the block retired before the ring knows of it can tell why
   uint32_t blocks_left = NANDLOG_PAGE_PLACEMENT_ATTEMPTS;
   if ((head_failed_page == NO_PAGE) || (block_of(head_failed_page) != block_of(current_page)))
   {
      head_failed_page = current_page;
      if (!write_boot_slot(0xFF))
         NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: The relocation of the write head's block %u could not be recorded in the metadata ring\n", block_of(current_page));
   }
   if (relocate_head_block(&blocks_left))
      return true;
   if (!log_region_full && !unwritable)
   {
      unwritable = true;
      NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: No block would take the write head's pages; logging disabled\n");
      nandlog_port_unwritable();
   }
   return false;
}

static void rewrite_passed_over_block(void)
{
   // The head's block holds nothing below the head but passed-over positions, which the slot now lists, and its blank
   // cells may not be reliably erased. Erase it again and give each passed-over position a tombstone. A cut part-way
   // through is harmless, because the positions stay consumed and the next boot does this again
   const uint32_t block = block_of(current_page), count = page_offset(current_page), first = head_index - count;
   if (nandlog_chip_erase_block(first_page_of(block)))
   {
      bool written = true;
      for (uint32_t i = 0; written && (i < count); ++i)
         written = write_tombstone(first_page_of(block) + i, first + i);
      if (written)
      {
         head_block_erased = true;
         return;
      }
   }

   // A block that will not erase, or take a tombstone, goes the way of any failing block
   relocate_at_boot();
}

static void prepare_write_head(void)
{
   // Make the recovered head safe to program from. This may erase or program, so it runs only after the ring holds
   // the boot's epoch and its passed-over list
   head_block_erased = false;
   window_blocks = 0;
   if (log_region_full)
      return;
   if ((head_failed_page != NO_PAGE) && (block_of(head_failed_page) == block_of(current_page)) && !block_retired(block_of(current_page)))
   {
      // A program failed in this block before the reboot, and a failed page may hold charge no read can see, so move
      // the block's pages before anything else is programmed
      NANDLOG_LOG(NANDLOG_LOG_WARNINGS, "WARNING: The write head's block %u refused a program before the reboot; relocating it\n", block_of(current_page));
      if (!relocate_at_boot())
         return;
   }
   else if (head_block_passed_over)
      rewrite_passed_over_block();
   else if (page_offset(current_page))
   {
      // The epoch is part-way through this block, so it is already erased. If the head's page is not blank, something
      // other than the log wrote it, and the block's pages move elsewhere
      head_block_erased = true;
      if ((verified_blank_page != current_page) && !nandlog_chip_page_is_blank(current_page))
      {
         NANDLOG_LOG(NANDLOG_LOG_WARNINGS, "WARNING: Write head at page %u is not blank; relocating its block\n", current_page);
         if (!relocate_at_boot())
            return;
      }
   }
   if (!log_region_full && !unwritable)
      establish_window();
}


// Committing a Page ---------------------------------------------------------------------------------------------------

static void build_page_image(nandlog_page_header_t *header)
{
   // A self-describing, self-validating page: ordering metadata and time bounds in the header, and separate
   // checksums over the header and the payload
   memset(header, 0, sizeof(*header));
   header->magic = NANDLOG_PAGE_MAGIC_THIS_BUILD;
   header->epoch = log_epoch;
   header->seq = head_index;
   header->first_timestamp = page_first_timestamp;
   header->last_timestamp = page_last_timestamp;
   header->payload_length = (uint16_t)cache_index;
   header->record_count = (uint16_t)page_record_count;
   header->payload_crc = cache_crc;
   check_crc("The page being committed", cache_crc, cache, cache_index);
   header->header_crc = crc32_compute(header, NANDLOG_PAGE_HEADER_CRC_BYTES);
   memset(transfer_buffer, 0xFF, page_size_bytes);
   memcpy(transfer_buffer, header, sizeof(*header));
   memcpy(transfer_buffer + sizeof(*header), cache, cache_index);
}

static void clear_page_assembly(void)
{
   // Nothing buffered: no records, no time bounds, and the CRC of no bytes
   cache_index = cache_crc = page_record_count = 0;
   page_first_timestamp = page_last_timestamp = NANDLOG_NO_TIMESTAMP;
}

static bool page_reads_back(uint32_t page, const nandlog_page_header_t *expected, const uint8_t *payload)
{
   // Read back the bytes written and compare them, which also catches a payload corrupted on the bus. A page the part
   // can only just correct straight after programming is not one to keep data in
   const uint32_t length = sizeof(*expected) + expected->payload_length;
   return (nandlog_chip_read_page_region(transfer_buffer, page, 0, length) == NANDLOG_READ_OK) &&
          !memcmp(transfer_buffer, expected, sizeof(*expected)) &&
          (!expected->payload_length || (payload && bytes_equal(transfer_buffer + sizeof(*expected), payload, expected->payload_length)));
}

static void build_tombstone(nandlog_page_header_t *header, uint32_t seq)
{
   // A tombstone keeps a position occupied when its page cannot be kept: a valid header with the position's sequence
   // number and no payload, laid out in transfer_buffer
   memset(header, 0, sizeof(*header));
   header->magic = NANDLOG_PAGE_MAGIC_THIS_BUILD;
   header->epoch = log_epoch;
   header->seq = seq;
   header->first_timestamp = header->last_timestamp = NANDLOG_NO_TIMESTAMP;
   header->payload_crc = crc32_compute(NULL, 0);
   header->header_crc = crc32_compute(header, NANDLOG_PAGE_HEADER_CRC_BYTES);
   memset(transfer_buffer, 0xFF, page_size_bytes);
   memcpy(transfer_buffer, header, sizeof(*header));
   ++stats.tombstones_written;
}

static bool write_tombstone(uint32_t page, uint32_t seq)
{
   nandlog_page_header_t header;
   if (NANDLOG_MUTATE_NO_TOMBSTONES)
      return true;
   build_tombstone(&header, seq);
   return nandlog_chip_write_page(transfer_buffer, page) && page_reads_back(page, &header, NULL);
}

static bool relocate_page(uint32_t source, uint32_t destination, uint32_t seq)
{
   // Move one page of a failing block, judged from a single read of the source, since a torn page can read differently
   // each time. This epoch's page for the position goes over as read, through the part's cache register if it reads
   // whole. Anything else goes over as a tombstone, because a copy of an unreadable page could come out blank and
   // mislead the head search. Every copy is read back from the target
   const nandlog_read_result_t read = nandlog_chip_read_page(transfer_buffer, source);
   nandlog_page_header_t header;
   memcpy(&header, transfer_buffer, sizeof(header));
   const bool own = (read != NANDLOG_READ_UNCORRECTABLE) && page_header_valid(&header) && (header.epoch == log_epoch) && (header.seq == seq);
   if (read == NANDLOG_READ_AT_ECC_LIMIT)
      ++stats.reads_at_ecc_limit;
   if (own && NANDLOG_CHIP_PAGE_COPY && (header.payload_crc == crc32_compute(transfer_buffer + sizeof(header), header.payload_length)))
   {
      // Relocation is where a failing part spends its time, so a whole page moves through the part itself. The copy
      // is checked by its header and by the part's verdict on the whole page
      const nandlog_copy_result_t copied = nandlog_chip_copy_page(source, destination);
      if (copied == NANDLOG_COPY_FAILED)
         return false;
      if (copied == NANDLOG_COPY_OK)
      {
         nandlog_page_header_t copy;
         return (nandlog_chip_read_page_region((uint8_t*)&copy, destination, 0, sizeof(copy)) == NANDLOG_READ_OK) &&
                !memcmp(&copy, &header, sizeof(copy));
      }
   }

   uint32_t length = sizeof(header);
   if (own)
      length += header.payload_length;
   else if (NANDLOG_MUTATE_NO_TOMBSTONES)
      return true;
   else
      build_tombstone(&header, seq);

   // The read-back lands in the buffer the image came from, so the image is remembered by its checksum
   const uint32_t image_crc = crc32_compute(transfer_buffer, length);
   return nandlog_chip_write_page(transfer_buffer, destination) &&
          (nandlog_chip_read_page_region(transfer_buffer, destination, 0, length) == NANDLOG_READ_OK) &&
          (crc32_compute(transfer_buffer, length) == image_crc);
}

static bool relocate_head_block(uint32_t *blocks_left)
{
   // The block under the write head will not take a page. Copy the epoch's pages in it to the next good block at the
   // same offsets, and only then retire the failing block. Until the retirement the copies sit one block ahead of their
   // own positions, so a power cut here leaves a log the head search still reads correctly
   const uint32_t failing = block_of(current_page), count = page_offset(current_page), first_seq = head_index - count;
   while (*blocks_left && !unwritable)
   {
      --*blocks_left;
      const uint32_t target = next_good_block(failing);
      if ((target == NO_BLOCK) || in_epoch_span(target))
      {
         // Every other block already belongs to the epoch: the region is full
         log_region_full = true;
         return false;
      }
      if (!erase_log_block(target))
         continue;
      bool copied = true;
      for (uint32_t i = 0; copied && (i < count); ++i)
         copied = relocate_page(first_page_of(failing) + i, first_page_of(target) + i, first_seq + i);
      if (!copied)
      {
         retire_block(target);
         continue;
      }
      relocation_target = target;
      const bool retired = retire_block(failing);
      relocation_target = NO_BLOCK;
      if (!retired)
         return false;
      stats.pages_relocated += count;
      current_page = first_page_of(target) + count;
      if (count)
         span_last_block = target;
      head_block_erased = true;

      // The window was laid out beyond the failing block, and the next call that commits nothing rebuilds it
      window_blocks = 0;
      return true;
   }
   return false;
}

static nandlog_status_t place_current_page(void)
{
   // Program the assembled page at the write head and verify it. A failure is never retried on the same page. The
   // head's block is relocated instead, and counts as the first of the NANDLOG_PAGE_PLACEMENT_ATTEMPTS blocks
   uint32_t blocks_left = NANDLOG_PAGE_PLACEMENT_ATTEMPTS - 1;
   for (;;)
   {
      if (!head_block_erased)
      {
         // The window never reached this block, so erase it now
         if (!erase_log_block(block_of(current_page)))
         {
            if (unwritable || !blocks_left-- || !move_head_past_retired_block())
               return log_region_full ? NANDLOG_ERR_FULL : NANDLOG_ERR_UNWRITABLE;
            continue;
         }
         head_block_erased = true;
      }

      nandlog_page_header_t header;
      build_page_image(&header);
      if (nandlog_chip_write_page(transfer_buffer, current_page) && page_reads_back(current_page, &header, cache))
         return NANDLOG_OK;

      // Record the failed page in the ring first, since it could read blank after a reboot and must not be programmed
      record_head_failure(current_page);
      if (!relocate_head_block(&blocks_left))
         return log_region_full ? NANDLOG_ERR_FULL : NANDLOG_ERR_UNWRITABLE;
   }
}

static void advance_write_head(void)
{
   // Step past the page at the head. A new block comes from the erased window, or is erased before its first program
   // if the window is empty
   span_last_block = block_of(current_page);
   ++head_index;
   if (head_index >= good_positions())
   {
      log_region_full = true;
      return;
   }
   if (page_offset(current_page + 1))
   {
      ++current_page;
      return;
   }
   const uint32_t block = next_good_block(block_of(current_page));
   if ((block == NO_BLOCK) || in_epoch_span(block))
   {
      log_region_full = true;
      return;
   }
   current_page = first_page_of(block);
   head_block_erased = (window_blocks > 0);
   if (window_blocks)
      --window_blocks;
}

static nandlog_status_t commit_current_page(void)
{
   // Write everything buffered as one page and reset the page-assembly state
   if (!cache_index)
      return NANDLOG_OK;
   part_awake();
   const nandlog_status_t status = place_current_page();
   if (status == NANDLOG_OK)
   {
      ++stats.pages_committed;
      advance_write_head();
   }
   else
   {
      stats.records_lost += page_record_count;
      if (status == NANDLOG_ERR_UNWRITABLE)
      {
         // Program nothing more until a reboot. The page at the head may read blank, so writing past it could leave a
         // blank position below the head
         if (!unwritable)
         {
            NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: No block would retain a log page after %u attempts; logging disabled\n", (uint32_t)NANDLOG_PAGE_PLACEMENT_ATTEMPTS);
            nandlog_port_unwritable();
         }
         unwritable = true;
      }
      else
         NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: The log region is full; %u records lost\n", page_record_count);
   }
   part_asleep();
   clear_page_assembly();
   return status;
}

static bool erase_ahead_due(void)
{
   // The window is short, and the head is past the middle of its block or the window is empty altogether
   return has_epoch && !log_region_full && !disabled && !unwritable &&
          (!head_block_erased || ((window_blocks < NANDLOG_ERASE_AHEAD_BLOCKS) &&
                                  (!window_blocks || (page_offset(current_page) >= erase_ahead_trigger_page))));
}

static void erase_ahead_once(void)
{
   // Erase one more block just past the window, in a call that committed no page, so that no call both programs a
   // page and erases a block
   part_awake();
   if (!head_block_erased)
   {
      if (erase_log_block(block_of(current_page)))
         head_block_erased = true;
      else if (!unwritable)
         move_head_past_retired_block();
   }
   else
   {
      uint32_t block = block_of(current_page);
      for (uint32_t i = 0; (i <= window_blocks) && (block != NO_BLOCK); ++i)
         block = next_good_block(block);
      if ((block != NO_BLOCK) && (block != block_of(current_page)) && !erase_refused(block) && erase_log_block(block))
         ++window_blocks;
   }
   part_asleep();

   // If the log can take no more pages, what is buffered can never be committed, so count it lost now
   if ((unwritable || log_region_full) && cache_index)
   {
      NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: The log stopped taking pages; %u buffered records lost\n", page_record_count);
      stats.records_lost += page_record_count;
      clear_page_assembly();
   }
}


// Metadata Ring -------------------------------------------------------------------------------------------------------

// The first METADATA_RING_BLOCKS blocks hold a slot per epoch, written in turn and wrapping. Each slot is written to
// both pages of a pair and numbered one more than the last, and ring blocks fill in order. A slot that is not whole in
// both copies may be torn, so a boot writes its choice again as a new slot, in a block of its own, before acting on it.
//
// A slot is read once, in a single read of its page, into an image in RAM. A torn page can read differently each
// time, so the boot decides by that image alone and builds every slot it writes from it

typedef struct __attribute__ ((__packed__))
{
   nandlog_meta_header_t header;
   nandlog_meta_extension_t extension;
} meta_slot_t;

typedef char nandlog_meta_slot_must_be_64_bytes[(sizeof(meta_slot_t) == 64) ? 1 : -1];

typedef enum
{
   IMAGE_BAD,                             // no slot: its header or extension does not check
   IMAGE_HEADER,                          // a slot, but its list of passed-over positions does not check
   IMAGE_LIST,                            // all but the details check: the caller's blob is lost, nothing else
   IMAGE_WHOLE                            // every checksum it carries holds
} image_state_t;

static bool meta_header_valid(const nandlog_meta_header_t *header)
{
   // The details are limited by what any build could fit on this part, not by this build's
   // NANDLOG_MAX_EPOCH_DETAILS_BYTES, so that a log written with a larger value is still found
   return (header->magic == NANDLOG_META_MAGIC) && (header->header_crc == crc32_compute(header, NANDLOG_META_HEADER_CRC_BYTES)) &&
          (header->details_length <= (page_size_bytes - sizeof(meta_slot_t) - (NANDLOG_MIN_PASSED_OVER_RUNS * sizeof(uint32_t)))) &&
          !page_offset(header->log_start_page) && in_log_region(block_of(header->log_start_page));
}

static void default_extension(nandlog_meta_extension_t *extension)
{
   // The extension of a slot with nothing to add: no failed head and nothing passed over
   memset(extension, 0, sizeof(*extension));
   extension->head_failed_page = NO_PAGE;
   extension->span_retired = 0;
   extension->sealed = 0xFF;
   extension->reserved = 0xFFFF;
   extension->passed_over_crc = crc32_compute(NULL, 0);
}

static bool read_slot(uint32_t page, meta_slot_t *slot)
{
   // A slot's header and extension, for ordering the ring and listing its epochs
   nandlog_chip_read_page_region((uint8_t*)slot, page, 0, sizeof(*slot));
   return meta_header_valid(&slot->header) && (slot->extension.extension_crc == crc32_compute(slot, NANDLOG_META_EXTENSION_CRC_BYTES));
}

static uint32_t slot_image_room(void)
{
   // A slot may fill its page whatever this build's limits, so that a slot written under other limits is read whole
   return page_size_bytes;
}

static image_state_t judge_slot_image(uint8_t *image)
{
   // Judge an image in RAM by every checksum it carries, without reading anything
   meta_slot_t slot;
   memcpy(&slot, image, sizeof(slot));
   if (!meta_header_valid(&slot.header))
      return IMAGE_BAD;
   const uint32_t list = slot.extension.passed_over_count * sizeof(uint32_t);
   if (slot.extension.extension_crc != crc32_compute(&slot, NANDLOG_META_EXTENSION_CRC_BYTES))
      return IMAGE_BAD;
   if (((sizeof(slot) + slot.header.details_length + list) > slot_image_room()) ||
       (crc32_compute(image + sizeof(slot) + slot.header.details_length, list) != slot.extension.passed_over_crc))
      return IMAGE_HEADER;
   return (crc32_compute(image + sizeof(slot), slot.header.details_length) == slot.header.details_crc) ? IMAGE_WHOLE : IMAGE_LIST;
}

static image_state_t load_slot_image(uint32_t page, uint8_t *image, bool *at_limit)
{
   // Read a slot once into 'image' and judge it. '*at_limit' is set if the part corrected that read only at the limit
   // of its ECC
   const nandlog_read_result_t read = nandlog_chip_read_page_region(image, page, 0, slot_image_room());
   if (read == NANDLOG_READ_AT_ECC_LIMIT)
      ++stats.reads_at_ecc_limit;
   if (at_limit)
      *at_limit = (read == NANDLOG_READ_AT_ECC_LIMIT);
#if NANDLOG_MUTATE_READ_SLOT_TWICE
   static uint8_t second_read[NANDLOG_MAX_PAGE_SIZE_BYTES];
   nandlog_chip_read_page_region(second_read, page, 0, slot_image_room());
   return judge_slot_image(second_read);
#else
   return judge_slot_image(image);
#endif
}

static void drop_details(uint8_t *image)
{
   // An image whose details did not check keeps everything else: the details go, and the list moves up to them
   meta_slot_t slot;
   memcpy(&slot, image, sizeof(slot));
   memmove(image + sizeof(slot), image + sizeof(slot) + slot.header.details_length, slot.extension.passed_over_count * sizeof(uint32_t));
   slot.header.details_length = 0;
   slot.header.details_crc = crc32_compute(NULL, 0);
   memcpy(image, &slot, sizeof(slot));
   NANDLOG_LOG(NANDLOG_LOG_WARNINGS, "WARNING: Epoch %u's details could not be read whole; it goes on without them\n", slot.header.epoch);
}

static void begin_slot_image(uint32_t epoch, uint32_t start_page, const void *details, uint16_t length)
{
   // A fresh image in the cache for an epoch: its header, a default extension, the details given, nothing passed over
   meta_slot_t slot;
   memset(&slot, 0, sizeof(slot));
   slot.header.magic = NANDLOG_META_MAGIC;
   slot.header.epoch = epoch;
   slot.header.log_start_page = start_page;
   slot.header.details_length = length;
   slot.header.format_version = NANDLOG_FORMAT_VERSION;
   slot.header.details_crc = crc32_compute(details, length);
   default_extension(&slot.extension);
   memcpy(cache, &slot, sizeof(slot));
   if (length)
      memcpy(cache + sizeof(slot), details, length);
   passed_over_list = cache + sizeof(slot) + length;
}

static bool read_slot_pair(uint32_t page, meta_slot_t *slot, uint32_t *found)
{
   // A slot is there if either of its copies is
   for (uint32_t copy = 0; copy < 2; ++copy)
      if (read_slot(page + copy, slot))
      {
         *found = page + copy;
         return true;
      }
   return false;
}

static bool slot_written(uint32_t slot)
{
   nandlog_meta_header_t header;
   nandlog_chip_read_page_region((uint8_t*)&header, slot, 0, sizeof(header));
   return !nandlog_bytes_erased((const uint8_t*)&header, sizeof(header));
}

static uint32_t ring_block_written_end(uint32_t block)
{
   // One past the last written page of a ring block, which is filled in order, by binary search
   uint32_t low = 0, high = pages_per_block;
   while (low < high)
   {
      const uint32_t mid = low + ((high - low) / 2);
      if (slot_written(first_page_of(block) + mid))
         low = mid + 1;
      else
         high = mid;
   }
   return low;
}

static uint32_t next_ring_block(uint32_t block)
{
   // The block the ring enters after 'block', stepping over bad ones
   for (uint32_t steps = 1; steps < METADATA_RING_BLOCKS; ++steps)
   {
      const uint32_t next = (block + steps) % METADATA_RING_BLOCKS;
      if (!nandlog_chip_is_bad_block(first_page_of(next)))
         return next;
   }
   return block;
}

static bool ring_block_entry(uint32_t block, meta_slot_t *slot, uint32_t *page)
{
   // The first readable slot of a ring block, which dates the block. It is usually on the block's first page
   if (nandlog_chip_is_bad_block(first_page_of(block)))
      return false;
   for (uint32_t offset = 0; offset < pages_per_block; ++offset)
   {
      if (read_slot(first_page_of(block) + offset, slot))
      {
         *page = first_page_of(block) + offset;
         return true;
      }
      if (nandlog_bytes_erased((const uint8_t*)&slot->header, sizeof(slot->header)))
         break;
   }
   return false;
}

static bool slot_newer(const meta_slot_t *a, const meta_slot_t *b)
{
   // Slots in the order they were written in
   return a->extension.sequence > b->extension.sequence;
}

static bool page_unwritten(uint32_t page)
{
   // Nothing programmed anywhere in the page, which a header read settles unless the header reads erased
   return !slot_written(page) && nandlog_chip_page_is_blank(page);
}

typedef enum
{
   SLOT_NONE,                             // nothing in the block a boot may go by
   SLOT_LOADED,                           // the boot's slot is in the cache
   SLOT_UNLOADABLE                        // the boot's slot lost its list, and only its head is in the cache
} slot_found_t;

static slot_found_t newest_slot_in_block(uint32_t block, uint32_t end, uint32_t *after, bool *both_whole, bool *at_limit)
{
   // Load into the cache the newest slot of a ring block that a boot may go by, keeping the better copy of each pair.
   // The last pair counts only if a copy is whole, since it may be torn and nothing acted on it. An earlier slot whose
   // list no longer reads cannot give way to an older one, so only its head is kept and the boot seals the epoch.
   // '*at_limit' says a copy of the chosen pair read only at the limit of the part's ECC
   const uint32_t pairs = (end + 1) / 2;
   for (uint32_t pair = pairs; pair--; )
   {
      const uint32_t page = first_page_of(block) + (2 * pair);
      bool first_limit = false, second_limit = false;
      const image_state_t first = load_slot_image(page, cache, &first_limit), second = load_slot_image(page + 1, transfer_buffer, &second_limit);
      image_state_t best = first;
      metadata_ring_page = page;
      if (second > first)
      {
         memcpy(cache, transfer_buffer, slot_image_room());
         metadata_ring_page = page + 1;
         best = second;
      }
      if ((best == IMAGE_BAD) || (((pair + 1) == pairs) && (best != IMAGE_WHOLE)))
         continue;
      *after = pairs - 1 - pair;
      *both_whole = (first == IMAGE_WHOLE) && (second == IMAGE_WHOLE);
      *at_limit = first_limit || second_limit;
      if (best == IMAGE_LIST)
         drop_details(cache);
      return (best == IMAGE_HEADER) ? SLOT_UNLOADABLE : SLOT_LOADED;
   }
   return SLOT_NONE;
}

static slot_found_t find_newest_metadata(bool *settled)
{
   // Find the newest slot a boot may trust and leave it in the cache, searching the ring blocks newest first, each
   // dated by its first readable slot
   meta_slot_t bound, entry;
   uint32_t top_block = NO_BLOCK, top_end = 0, after = 0;
   bool both_whole = false, at_limit = false;
   slot_found_t found = SLOT_NONE;
   *settled = false;
   for (uint32_t round = 0; (found == SLOT_NONE) && (round < METADATA_RING_BLOCKS); ++round)
   {
      uint32_t block = NO_BLOCK, entry_page = NO_PAGE;
      for (uint32_t candidate = 0; candidate < METADATA_RING_BLOCKS; ++candidate)
      {
         meta_slot_t slot;
         uint32_t page;
         if (ring_block_entry(candidate, &slot, &page) && ((top_block == NO_BLOCK) || slot_newer(&bound, &slot)) &&
             ((block == NO_BLOCK) || slot_newer(&slot, &entry)))
         {
            entry = slot;
            block = candidate;
            entry_page = page;
         }
      }
      if (block == NO_BLOCK)
         break;
      bound = entry;
      if (top_block == NO_BLOCK)
      {
         // A torn or failed slot written after the newest readable one may read one day. A slot that settles the
         // matter goes into the next block, numbered past them all, so that each block's written pages stay a prefix
         top_block = block;
         top_end = ring_block_written_end(block);
         next_ring_slot = first_page_of(next_ring_block(block));
         next_slot_sequence = entry.extension.sequence + ((top_end + 1) / 2) - (page_offset(entry_page) / 2) + 1;
      }
      found = newest_slot_in_block(block, (block == top_block) ? top_end : ring_block_written_end(block), &after, &both_whole, &at_limit);
   }
   if (found != SLOT_LOADED)
      return found;

   // The newest slot is settled, and left where it is, if it is the last thing written in the newest block, both its
   // copies are whole and read with margin, and nothing was written after it. Both copies share a block that every
   // boot reads, so they wear together. A copy at the limit of the part's ECC therefore leaves the slot unsettled, and
   // the boot writes it again into a fresh block
   const uint32_t next = next_ring_block(top_block);
   *settled = (block_of(metadata_ring_page) == top_block) && !after && both_whole && !at_limit &&
              ((top_end == pages_per_block) || page_unwritten(first_page_of(top_block) + top_end));
   if (*settled && (next != top_block))
   {
      meta_slot_t slot;
      *settled = page_unwritten(first_page_of(next)) || read_slot(first_page_of(next), &slot);
   }
   if (*settled)
   {
      next_ring_slot = (top_end < pages_per_block) ? (first_page_of(top_block) + top_end) : first_page_of(next);
      next_slot_sequence = ((const meta_slot_t*)cache)->extension.sequence + 1;
   }
   return SLOT_LOADED;
}

static uint32_t span_fingerprint(uint32_t start_block, uint32_t span_blocks, uint32_t excluded)
{
   // A fingerprint of which of an epoch's first 'span_blocks' blocks are retired: the sum of the CRCs of their offsets
   // from its first block
   uint32_t sum = 0;
   if (retired_list_complete)
   {
      for (uint32_t i = 0; i < retired_log_blocks; ++i)
      {
         const uint32_t offset = log_block_distance(start_block, retired_blocks[i]);
         if ((offset < span_blocks) && (retired_blocks[i] != excluded))
            sum += crc32_compute(&offset, sizeof(offset));
      }
      return sum;
   }
   for (uint32_t offset = 0, block = start_block; offset < span_blocks; ++offset, block = log_block_after(block))
      if ((block != excluded) && nandlog_chip_is_bad_block(first_page_of(block)))
         sum += crc32_compute(&offset, sizeof(offset));
   return sum;
}

static uint32_t current_span_blocks(void)
{
   return (has_epoch && head_index) ? (log_block_distance(block_of(starting_page), span_last_block) + 1) : 0;
}

typedef enum
{
   FROM_CACHE,                            // the image a boot loaded, or begin_epoch built, in the cache
   FROM_RING                              // the current slot, loaded from the ring
} slot_source_t;

static bool stage_slot_image(slot_source_t source)
{
   // The slot to write, in transfer_buffer: from the cache, or from the current slot read once and judged
   if (source == FROM_CACHE)
   {
      meta_slot_t slot;
      memcpy(&slot, cache, sizeof(slot));
      memcpy(transfer_buffer, cache, sizeof(slot) + slot.header.details_length + (slot.extension.passed_over_count * sizeof(uint32_t)));
      return true;
   }
   // Prefer a whole copy, or else a copy whose list is whole, without its details. Each copy is read once and judged
   for (uint32_t pass = 0; pass < 2; ++pass)
      for (uint32_t copy = 0; (copy < 2) && (metadata_ring_page != NO_PAGE); ++copy)
      {
         const image_state_t state = load_slot_image(metadata_ring_page ^ copy, transfer_buffer, NULL);
         if ((state == (pass ? IMAGE_LIST : IMAGE_WHOLE)) && (((const meta_slot_t*)transfer_buffer)->header.epoch == log_epoch))
         {
            if (state == IMAGE_LIST)
               drop_details(transfer_buffer);
            return true;
         }
      }
   return false;
}

static bool write_metadata_slot(slot_source_t source, uint32_t failed_page)
{
   // Program the next pair of the ring with the staged slot, numbered afresh and carrying the current retired blocks,
   // and verify both copies byte for byte and with ECC margin. A pair that is not blank or will not program ends the
   // use of its block, so that every block's written pages stay a prefix of it, as the boot's search relies on. If no
   // pair takes both copies, a single verified copy still counts as written, since a reboot would go by it
   uint32_t page = next_ring_slot + (page_offset(next_ring_slot) & 1), single = NO_PAGE, single_failed = NO_PAGE;
   for (uint32_t tries = 0; tries <= (2 * METADATA_RING_BLOCKS); ++tries)
   {
      if (page >= metadata_ring_pages)
         page = 0;
      const uint32_t block = block_of(page), next_block_page = first_page_of((block + 1) % METADATA_RING_BLOCKS);
      if (nandlog_chip_is_bad_block(page) || ((page_offset(page) + 2) > pages_per_block))
      {
         page = next_block_page;
         continue;
      }
      if (!page_offset(page))
      {
         // Never erase the block holding the current epoch's slot, which is all a reboot has to go on
         if ((has_epoch && (block_of(metadata_ring_page) == block)) || ((single != NO_PAGE) && (block_of(single) == block)))
         {
            NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: The metadata ring has no block to spare\n");
            break;
         }
         if (!nandlog_chip_erase_block(page))
         {
            if (nandlog_chip_mark_bad_block(page) && nandlog_chip_is_bad_block(page))
               track_retired_block(block);
            page = next_block_page;
            continue;
         }
      }
      else if (!nandlog_chip_page_is_blank(page))
      {
         page = next_block_page;
         continue;
      }

      // Stage the image for every attempt, because a failed attempt's read-back overwrites it
      if (!stage_slot_image(source))
      {
         NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: The current epoch's slot cannot be read whole; nothing written\n");
         return false;
      }
      meta_slot_t slot;
      memcpy(&slot, transfer_buffer, sizeof(slot));
      const uint32_t list = slot.extension.passed_over_count * sizeof(uint32_t);
      const uint32_t length = sizeof(slot) + slot.header.details_length + list;
      slot.header.magic = NANDLOG_META_MAGIC;
      slot.header.created_timestamp = 0;
      slot.header.header_crc = crc32_compute(&slot.header, NANDLOG_META_HEADER_CRC_BYTES);
      slot.header.reserved = 0xFFFFFFFF;
      if (source == FROM_RING)
         slot.extension.head_failed_page = failed_page;
      slot.extension.sequence = next_slot_sequence++;

      // If the bad-block table no longer lays the epoch out as written, keep the slot's original record, so that later
      // boots find the disagreement too and a new epoch still begins past the old one's blocks (block_past_epoch())
      if (!layout_unverified || (slot.header.epoch != log_epoch))
      {
         slot.extension.retired_blocks = bad_block_count;
         slot.extension.span_blocks = (uint16_t)((slot.header.epoch == log_epoch) ? current_span_blocks() : 0);
         slot.extension.span_retired = span_fingerprint(block_of(slot.header.log_start_page), slot.extension.span_blocks, NO_BLOCK);
      }
      slot.extension.reserved = 0xFFFF;
      slot.extension.passed_over_crc = crc32_compute(transfer_buffer + sizeof(slot) + slot.header.details_length, list);
      slot.extension.extension_crc = crc32_compute(&slot, NANDLOG_META_EXTENSION_CRC_BYTES);
      memcpy(transfer_buffer, &slot, sizeof(slot));
      memset(transfer_buffer + length, 0xFF, page_size_bytes - length);
      if (judge_slot_image(transfer_buffer) != IMAGE_WHOLE)
      {
         NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: A metadata slot failed its own checksums; nothing written\n");
         return false;
      }
      const uint32_t image_crc = crc32_compute(transfer_buffer, length);
      uint32_t copies = 0;
      for (uint32_t copy = 0; (copies == copy) && (copy < 2); ++copy)
         if (nandlog_chip_write_page(transfer_buffer, page + copy) && (nandlog_chip_read_page_region(transfer_buffer, page + copy, 0, length) == NANDLOG_READ_OK) &&
             (crc32_compute(transfer_buffer, length) == image_crc))
         {
            memset(transfer_buffer + length, 0xFF, page_size_bytes - length);
            ++copies;
         }
      if (copies == 2)
      {
         metadata_ring_page = page;
         next_ring_slot = page + 2;
         head_failed_page = slot.extension.head_failed_page;
         return true;
      }
      if (copies && (single == NO_PAGE))
      {
         single = page;
         single_failed = slot.extension.head_failed_page;
      }
      page = next_block_page;
   }
   if (single == NO_PAGE)
      return false;
   NANDLOG_LOG(NANDLOG_LOG_WARNINGS, "WARNING: The metadata ring holds the new slot once only\n");
   metadata_ring_page = single;
   next_ring_slot = first_page_of((block_of(single) + 1) % METADATA_RING_BLOCKS);
   head_failed_page = single_failed;
   return true;
}

static bool rewrite_current_slot(uint32_t failed_page)
{
   // Write the current epoch's slot again, from the ring, as the newest slot: to record a failed head or a
   // retirement. A boot writes its own image from the cache instead (write_boot_slot())
   return write_metadata_slot(FROM_RING, failed_page);
}

static void record_head_failure(uint32_t page)
{
   if (!rewrite_current_slot(page))
      NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: The failed write head at page %u could not be recorded in the metadata ring\n", page);
}

static uint32_t list_epochs(uint32_t *epochs, uint32_t *ring_pages, uint32_t capacity)
{
   // List epochs newest first, walking the ring back from the current epoch's slot a pair at a time. Slots can repeat
   // an epoch or, if torn, name a later one, so each epoch listed must be older than the last
   if (!has_epoch)
      return 0;
   meta_slot_t slot;
   uint32_t found = 0, page = 0, block = block_of(metadata_ring_page), end = page_offset(metadata_ring_page) + 1;
   for (uint32_t visited = 0; (visited < METADATA_RING_BLOCKS) && (found < capacity); ++visited)
   {
      for (uint32_t offset = ((end + 1) / 2) * 2; (offset >= 2) && (found < capacity); offset -= 2)
         if (read_slot_pair(first_page_of(block) + offset - 2, &slot, &page) && (!found || (slot.header.epoch < epochs[found - 1])))
         {
            epochs[found] = slot.header.epoch;
            ring_pages[found++] = page;
         }

      // On to the block before, and only as far into it as it was written
      block = (block + METADATA_RING_BLOCKS - 1) % METADATA_RING_BLOCKS;
      end = ring_block_entry(block, &slot, &page) ? ring_block_written_end(block) : 0;
   }
   return found;
}


// Reading -------------------------------------------------------------------------------------------------------------

static inline uint32_t view_epoch(void)      { return epoch_is_current ? log_epoch : selected_epoch; }
static inline uint32_t view_start_page(void) { return epoch_is_current ? starting_page : selected_start_page; }
static inline uint32_t view_ring_page(void)  { return epoch_is_current ? metadata_ring_page : selected_ring_page; }

// A read counts its pages by sequence number, which is a page's position among the good pages of its epoch

static inline uint32_t view_positions(void)
{
   // Pages the view's epoch has written
   return epoch_is_current ? (has_epoch ? head_index : 0) : selected_positions;
}

static uint32_t view_page(uint32_t index)
{
   // The page holding the view's index-th sequence number
   return page_at_position(view_start_page(), index);
}

static uint32_t read_view_page(uint32_t page, uint8_t *buffer, nandlog_page_header_t *stored, bool *header_known)
{
   // Read and verify one page of the view, returning its payload length, or zero for a gap or another epoch's page.
   // The payload is copied into 'buffer' if one is given, and '*header_known' says whether 'stored' can be trusted
   const nandlog_read_result_t read = nandlog_chip_read_page(transfer_buffer, page);
   memcpy(stored, transfer_buffer, sizeof(*stored));
   *header_known = page_header_valid(stored) && (stored->epoch == view_epoch());
   if (!*header_known || (read == NANDLOG_READ_UNCORRECTABLE))
      return 0;
   if (read == NANDLOG_READ_AT_ECC_LIMIT)
      ++stats.reads_at_ecc_limit;
   const uint32_t length = stored->payload_length;
   if (!length || (stored->payload_crc != crc32_compute(transfer_buffer + sizeof(*stored), length)))
      return 0;
   if (buffer)
      memcpy(buffer, transfer_buffer + sizeof(*stored), length);
   return length;
}

static inline uint32_t time_bound(uint32_t timestamp)
{
   // NANDLOG_NO_TIMESTAMP is also a time a record can carry, so in a bound it counts as one less, and the page holding
   // such a record is still found
   return (timestamp == NANDLOG_NO_TIMESTAMP) ? (NANDLOG_NO_TIMESTAMP - 1) : timestamp;
}

static uint32_t seek_position_for_timestamp(uint32_t timestamp, bool at_or_after)
{
   // Resolve a time boundary to a position by examining every page header in the view, rather than by binary search,
   // because time steps back wherever the time base changed. Returns the view's position count if no page qualifies
   const uint32_t total = view_positions();
   uint32_t result = total;
   uint32_t page = total ? view_page(0) : NO_PAGE;
   timestamp = time_bound(timestamp);
   for (uint32_t position = 0; position < total; ++position)
   {
      nandlog_page_header_t header;
      nandlog_chip_read_page_region((uint8_t*)&header, page, 0, sizeof(header));
      page = next_good_page(page);
      if (!page_header_valid(&header) || (header.epoch != view_epoch()))
         continue;
      const uint32_t first = header.first_timestamp, last = header.last_timestamp;
      if (at_or_after)
      {
         // Scanning forward, so the first page whose largest timestamp reaches the bound is the answer
         if ((last != NANDLOG_NO_TIMESTAMP) && (last >= timestamp))
            return position;
      }
      else if ((first != NANDLOG_NO_TIMESTAMP) && (first <= timestamp))
         result = position;            // keep the last page whose smallest timestamp is within the bound
   }
   return result;
}

static uint32_t earlier_epoch_positions(uint32_t epoch, uint32_t start_page, uint32_t successor_start_page)
{
   // An earlier epoch ends at the last page it wrote, in the last good block before its successor began or the block
   // before that. Returns 0 if neither holds a page of the epoch, since the current epoch has swept it away. A
   // successor that begins no good block after this epoch's start means this epoch filled the region
   uint32_t good_blocks = position_of_page(start_page, successor_start_page) / pages_per_block;
   if (!good_blocks)
      good_blocks = log_block_count - retired_log_blocks;
   for (uint32_t back = 1; (back <= 2) && (back <= good_blocks); ++back)
   {
      const uint32_t block_index = good_blocks - back, block = good_block_at(block_of(start_page), block_index);
      if (block == NO_BLOCK)
         break;

      // Search back from the block's end rather than by binary search, since a passed-over page may read blank
      for (uint32_t offset = pages_per_block; offset-- > 0; )
      {
         nandlog_page_header_t header;
         nandlog_chip_read_page_region((uint8_t*)&header, first_page_of(block) + offset, 0, sizeof(header));
         if (page_header_valid(&header) && (header.epoch == epoch))
            return (block_index * pages_per_block) + offset + 1;
      }
   }
   return 0;
}

static bool find_position_by_seq(uint32_t seq, uint32_t *position)
{
   // In the current epoch a sequence number is its page's position. In an earlier one, a block retired since may have
   // shifted the positions, so the guess is checked and, failing that, the sequence number is found by binary search
   const uint32_t positions = view_positions();
   if (epoch_is_current || (seq < positions))
   {
      *position = seq;
      if (epoch_is_current)
         return seq < positions;
      nandlog_page_header_t header;
      nandlog_chip_read_page_region((uint8_t*)&header, view_page(seq), 0, sizeof(header));
      if (page_header_valid(&header) && (header.epoch == view_epoch()) && (header.seq == seq))
         return true;
   }
   uint32_t low = 0, high = positions;
   while (low < high)
   {
      const uint32_t mid = low + ((high - low) / 2);
      uint32_t probe = mid, page = view_page(mid);
      nandlog_page_header_t header;
      bool valid = false;
      for (; !valid && (probe < high); page = next_good_page(page), ++probe)
      {
         nandlog_chip_read_page_region((uint8_t*)&header, page, 0, sizeof(header));
         valid = page_header_valid(&header) && (header.epoch == view_epoch());
      }
      if (!valid)
         high = mid;
      else if (header.seq == seq)
      {
         *position = probe - 1;
         return true;
      }
      else if (header.seq < seq)
         low = probe;
      else
         high = mid;
   }
   return false;
}


// Locked Implementations ----------------------------------------------------------------------------------------------

static bool nandlog_probe_locked(void)
{
   // Presence only: bring the port up, ask the part to identify itself, and put everything back to sleep
   if (!nandlog_port_init())
      return false;
   const bool chip_present = nandlog_chip_probe();
   nandlog_chip_low_power(true);
   nandlog_port_power(false);
   nandlog_port_write_enable(false);
   return chip_present;
}

// The Bad-Block Table's Blocks ----------------------------------------------------------------------------------------

// A driver whose bad-block table has run out of blocks of its own asks for a run of blocks from the top of the log
// region. A boot gives them once it knows where the current epoch lies and before it writes anything, and so does a
// retirement that cannot otherwise be recorded (retire_block()). A block given lies outside the epoch's positions, is
// not the head's, and holds no valid page of any epoch, so retiring it moves no position below the head

static bool block_holds_nothing(uint32_t block)
{
   // Whether no page of the block carries a valid header of any epoch
   for (uint32_t offset = 0; offset < pages_per_block; ++offset)
   {
      nandlog_page_header_t header;
      nandlog_chip_read_page_region((uint8_t*)&header, first_page_of(block) + offset, 0, sizeof(header));
      if (page_header_valid(&header))
         return false;
   }
   return true;
}

static bool block_may_be_given(uint32_t block)
{
   // A good block of the region, outside the epoch's positions, not the head's, and not one a retirement in progress
   // is retiring or copying into
   const uint32_t head_block = (has_epoch && (current_page != NO_PAGE)) ? block_of(current_page) : NO_BLOCK;
   return in_log_region(block) && !block_retired(block) && !in_epoch_span(block) && (block != head_block) &&
          (block != block_being_retired) && (block != relocation_target);
}

static bool give_blocks_to_the_table(void)
{
   // Give the table the highest aligned run of blocks that may be given and hold nothing, trying a few runs at most.
   // Blocks in the range holding anything but log pages are erased first, so that a torn table is never trusted
   uint32_t lowest_page = 0;
   const uint32_t wanted = nandlog_chip_table_blocks_wanted(&lowest_page);
   if (!wanted || (wanted > log_block_count))
      return false;
   const uint32_t floor = (block_of(lowest_page) > log_first_block) ? block_of(lowest_page) : log_first_block;
   for (uint32_t block = log_end_block; block-- > floor; )
   {
      nandlog_page_header_t header;
      if (!block_may_be_given(block))
         continue;
      nandlog_chip_read_page_region((uint8_t*)&header, first_page_of(block), 0, sizeof(header));
      if (!page_header_valid(&header) && !nandlog_chip_page_is_blank(first_page_of(block)) && block_holds_nothing(block))
         nandlog_chip_erase_block(first_page_of(block));
   }
   uint32_t attempts = NANDLOG_PAGE_PLACEMENT_ATTEMPTS;
   for (uint32_t first = ((log_end_block - wanted) / wanted) * wanted; attempts && (first >= floor); first -= wanted)
   {
      bool fits = true;
      for (uint32_t i = 0; fits && (i < wanted); ++i)
         fits = block_may_be_given(first + i) && block_holds_nothing(first + i);
      if (fits)
      {
         --attempts;
         const bool taken = nandlog_chip_table_take_blocks(first_page_of(first));
         for (uint32_t i = 0; i < wanted; ++i)
            if (!block_retired(first + i) && nandlog_chip_is_bad_block(first_page_of(first + i)))
               track_retired_block(first + i);
         window_blocks = 0;
         if (taken)
         {
            NANDLOG_LOG(NANDLOG_LOG_INFO, "INFO: Blocks %u-%u given to the bad-block table\n", first, first + wanted - 1);
            return true;
         }
      }
      if (first < wanted)
         break;
   }
   NANDLOG_LOG(NANDLOG_LOG_WARNINGS, "WARNING: No blocks for the bad-block table\n");
   return false;
}

static void stop_writing(const char *message)
{
   NANDLOG_LOG(NANDLOG_LOG_ERRORS, "%s", message);
   unwritable = true;
   nandlog_port_unwritable();
}

static bool write_boot_slot(uint8_t sealed)
{
   // Write the boot's image, with its grown list and failed head, as the newest slot
   meta_slot_t slot;
   memcpy(&slot, cache, sizeof(slot));
   slot.extension.head_failed_page = head_failed_page;
   slot.extension.sealed = sealed;
   slot.extension.passed_over_count = (uint16_t)passed_over_count;
   memcpy(cache, &slot, sizeof(slot));
   return write_metadata_slot(FROM_CACHE, head_failed_page);
}

static void seal_epoch(const char *why)
{
   // The epoch takes no more pages until a new one begins, and its slot tells every later boot so. Reads still work
   NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: %s; epoch %u is sealed, and logging disabled until a new epoch is begun\n", why, log_epoch);
   if (!write_boot_slot(0))
      NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: The seal could not be written to the metadata ring\n");
   sealed = unwritable = true;
   nandlog_port_unwritable();
}

static bool table_agrees_with_ring(const meta_slot_t *slot)
{
   // The table must lay the epoch's written positions out as it did when the slot was written: no retirement the ring
   // knew of missing, and none new under them. The exception is the block of a failed head, whose retirement completes
   // a relocation the slot recorded, once the head is at or past the failed page. A head found short means a block
   // under committed pages was retired without the log, and erasing past the head would erase those pages
   if ((slot->extension.retired_blocks > bad_block_count) || head_found_short)
      return false;
   const uint32_t start = block_of(slot->header.log_start_page), span = slot->extension.span_blocks;
   if (span_fingerprint(start, span, NO_BLOCK) == slot->extension.span_retired)
      return true;
   const uint32_t failed = (slot->extension.head_failed_page != NO_PAGE) ? block_of(slot->extension.head_failed_page) : NO_BLOCK;
   return (failed != NO_BLOCK) && block_retired(failed) && (span_fingerprint(start, span, failed) == slot->extension.span_retired) &&
          (head_index >= position_of_page(slot->header.log_start_page, slot->extension.head_failed_page));
}

static void recover_epoch(slot_found_t found, bool slot_settled)
{
   // Find the write head by reading only, continue any newer epoch whose slot was lost, and write the boot's slot
   // before anything in the log region is erased or programmed. If the slot no longer reads whole, the table lays the
   // epoch out differently, or its list is full, the epoch is sealed in its slot
   meta_slot_t slot;
   memcpy(&slot, cache, sizeof(slot));
   bool adopted = false, added = false, listed = true;
   list_lost = (found != SLOT_LOADED);
   passed_over_count = list_lost ? 0 : slot.extension.passed_over_count;
   passed_over_list = cache + sizeof(slot) + slot.header.details_length;
   locate_write_head();

   // Continue a newer epoch whose slot was lost before acting on this slot, since that is the epoch the log was writing
   if (!adopt_lost_epochs(&adopted))
   {
      unwritable = true;
      nandlog_port_unwritable();
      return;
   }
   layout_unverified = !adopted && !table_agrees_with_ring(&slot);
   if (!adopted && list_lost)
   {
      slot.header.details_length = 0;
      slot.header.details_crc = crc32_compute(NULL, 0);
      slot.extension.passed_over_count = 0;
      memcpy(cache, &slot, sizeof(slot));
      passed_over_list = cache + sizeof(slot);
      seal_epoch(NANDLOG_LOG_TEXT(NANDLOG_LOG_ERRORS, "The newest slot the epoch acted on no longer reads whole"));
      return;
   }
   if (!adopted && (slot.extension.sealed != 0xFF))
   {
      sealed = true;
      stop_writing(NANDLOG_LOG_TEXT(NANDLOG_LOG_ERRORS, "ERROR: The epoch is sealed; logging disabled until a new epoch is begun\n"));
      return;
   }
   if (layout_unverified)
   {
      NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: The bad-block table lists %u retired blocks where the metadata ring recorded %u, or other ones under the epoch\n",
                  bad_block_count, slot.extension.retired_blocks);
      seal_epoch(NANDLOG_LOG_TEXT(NANDLOG_LOG_ERRORS, "The bad-block table no longer lays the epoch out as it was written"));
      return;
   }
   if (!adopted && (slot.header.details_length > NANDLOG_MAX_EPOCH_DETAILS_BYTES))
   {
      // Written with a larger NANDLOG_MAX_EPOCH_DETAILS_BYTES, so the slot has less room for the list than needed
      seal_epoch(NANDLOG_LOG_TEXT(NANDLOG_LOG_ERRORS, "The epoch's slot carries longer details than this build allows"));
      return;
   }

   // List the positions passed over below the head before anything is committed past them. A list without room for
   // another boot's worth seals the epoch
   for (uint32_t position = passed_over_from; listed && (position < head_index); ++position)
      listed = pass_over(position, &added);
   if (NANDLOG_PASS_OVER_HEAD_AT_BOOT && listed && !log_region_full && page_offset(current_page) && !head_block_passed_over &&
       ((head_failed_page == NO_PAGE) || (block_of(head_failed_page) != block_of(current_page))) && pass_over(head_index, &added))
   {
      // Pass over the head's page too, since a cut program may have left it charge no read can see. This is not
      // needed at a block boundary, where the block is erased again, or in a block a program failed in
      span_last_block = block_of(current_page);
      ++head_index;
      ++stats.positions_skipped;
      if (head_index >= good_positions())
         log_region_full = true;
      else if (page_offset(current_page + 1))
         ++current_page;
      else
      {
         const uint32_t block = next_good_block(block_of(current_page));
         log_region_full = (block == NO_BLOCK) || in_epoch_span(block);
         current_page = log_region_full ? NO_PAGE : first_page_of(block);
      }
   }
   if (!listed || ((passed_over_count + NANDLOG_PASSED_OVER_HEADROOM) > passed_over_capacity))
   {
      seal_epoch(NANDLOG_LOG_TEXT(NANDLOG_LOG_ERRORS, "The epoch has passed over as many runs of positions as its slot can list"));
      return;
   }

   // The epoch's positions are known and nothing has been written, so the driver's table can safely be given blocks
   give_blocks_to_the_table();
   if ((!slot_settled || adopted || added || (slot.extension.retired_blocks != bad_block_count)) &&
       !write_boot_slot(0xFF))
   {
      stop_writing(NANDLOG_LOG_TEXT(NANDLOG_LOG_ERRORS, "ERROR: The metadata ring would not take the current epoch's slot; logging disabled\n"));
      return;
   }
   prepare_write_head();
}

static bool nandlog_init_locked(void)
{
   // Return if already initialized
   if (is_initialized)
      return true;

   // A compiler that could not say what byte order it builds for is checked here instead
   const uint32_t byte_order = 1;
   if (*(const uint8_t*)&byte_order != 1)
   {
      NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: nandlog requires a little-endian target\n");
      return false;
   }

   // Start from a clean RAM state, as after a power cut
   in_session = is_reading = disabled = unwritable = sealed = list_lost = layout_unverified = has_epoch = log_region_full = false;
   epoch_is_current = true;
   clear_page_assembly();
   retransmit_num_pages = 0;
   memset(&stats, 0, sizeof(stats));

   // Everything platform-specific about reaching the chip lives behind this call
   if (!nandlog_port_init())
      return false;

   // Resolve the log region from the geometry the chip reports before anything can consult it
   if (!resolve_geometry())
      return false;

   // Confirm the chip is there and answering before anything is asked of it
   if (!nandlog_chip_probe())
      return false;

   // Configure the chip and load or build its bad-block table
   nandlog_chip_init();
   build_retired_list();

   // Recover the current epoch from the metadata ring, then locate its write head
   bool settled = true;
   metadata_ring_page = current_page = head_failed_page = NO_PAGE;
   next_ring_slot = head_index = window_blocks = passed_over_count = 0;
   next_slot_sequence = 1;
   epoch_floor = 0;
   head_block_erased = head_block_passed_over = false;
   const slot_found_t found = find_newest_metadata(&settled);
   has_epoch = (found != SLOT_NONE);
   if (has_epoch)
   {
      meta_slot_t slot;
      memcpy(&slot, cache, sizeof(slot));
      log_epoch = slot.header.epoch;
      starting_page = slot.header.log_start_page;
      head_failed_page = slot.extension.head_failed_page;
      recover_epoch(found, settled);
   }
   else
   {
      // Nothing valid in the ring: a factory-fresh part, or one written in a format this cannot read. Nothing is
      // written until the first epoch begins, except blocks given to the driver's bad-block table
      log_epoch = 0;
      starting_page = first_page_of(log_first_block);
      give_blocks_to_the_table();
   }

   // Put the storage peripheral into Deep Sleep mode and disable writes
   nandlog_chip_low_power(true);
   nandlog_port_power(false);
   nandlog_port_write_enable(false);
   is_initialized = true;
   return true;
}

static void nandlog_deinit_locked(void)
{
   // Only continue if initialized
   if (!is_initialized)
      return;

   // Disable all chip communications
   part_awake();
   nandlog_port_deinit();
   is_reading = in_session = is_initialized = false;
}

static void nandlog_reset_bad_block_table_locked(void)
{
   // RECOVERY UTILITY. Discards the persisted bad-block table so it is rebuilt on the next boot. How much of
   // that is possible depends on the part, so the chip driver decides and reports whether it succeeded
   part_awake();
   const bool cleared = nandlog_chip_reset_bad_blocks();
   part_asleep();
   if (cleared)
      NANDLOG_LOG(NANDLOG_LOG_INFO, "INFO: Bad-block table erased and verified clear; rebuilt on next boot\n");
   else
      NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: Bad-block table NOT cleared\n");
}

static uint32_t block_past_epoch(void)
{
   // The bad-block table no longer lays the current epoch out as written, so its head is unreliable. Begin the new
   // epoch past the last block that holds a page of it, so that nothing of it is erased
   uint32_t block = block_of(starting_page), last = NO_BLOCK;
   for (uint32_t steps = 0; steps < log_block_count; ++steps, block = log_block_after(block))
      for (uint32_t offset = 0; (offset < 2) && !block_retired(block); ++offset)
      {
         nandlog_page_header_t header;
         nandlog_chip_read_page_region((uint8_t*)&header, first_page_of(block) + offset, 0, sizeof(header));
         if (page_header_valid(&header) && (header.epoch == log_epoch))
         {
            last = block;
            break;
         }
      }
   return (last == NO_BLOCK) ? good_block_at(block_of(starting_page), 0) : next_good_block(last);
}

static bool nandlog_begin_epoch_locked(const void *blob, uint16_t length)
{
   // Ensure that the metadata length is within the expected bounds
   if (!is_initialized || (length > NANDLOG_MAX_EPOCH_DETAILS_BYTES))
      return false;

   // Refuse loudly rather than silently discarding the write
   if (!in_session || is_reading)
   {
      NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: Refusing to begin an epoch outside of a session or during a read\n");
      return false;
   }
   epoch_is_current = true;

   // Records still buffered belong to the epoch that is ending, so they are committed to it first
   if (has_epoch && cache_index && (commit_current_page() != NANDLOG_OK))
   {
      NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: Refusing to begin an epoch; the records buffered for the last one could not be committed\n");
      return false;
   }

   // The new epoch continues the wear sweep from the old one's write head. A full epoch is followed from its own
   // first block
   uint32_t new_start;
   if (!has_epoch)
      new_start = block_retired(log_first_block) ? next_good_block(log_first_block) : log_first_block;
   else if (layout_unverified)
      new_start = block_past_epoch();
   else if (log_region_full)
      new_start = good_block_at(block_of(starting_page), 0);
   else if (!page_offset(current_page))
      new_start = block_of(current_page);
   else
      new_start = next_good_block(block_of(current_page));
   if (new_start == NO_BLOCK)
      return false;

   // The slot is the commit point, and the new epoch's blocks are erased only after it is written. Nothing is buffered
   // now, so the slot's image can be built in the cache
   const uint32_t epoch = has_epoch ? (((epoch_floor > log_epoch) ? epoch_floor : log_epoch) + 1) : 1;
   begin_slot_image(epoch, first_page_of(new_start), blob, length);
   if (!write_metadata_slot(FROM_CACHE, NO_PAGE))
   {
      NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: Unable to write metadata to any slot in the ring\n");
      return false;
   }

   // The new epoch is now live. Nothing will be written past the old epoch's head again, so a part that stopped
   // retaining pages there may try again here
   unwritable = sealed = list_lost = layout_unverified = false;
   log_epoch = epoch;
   starting_page = current_page = first_page_of(new_start);
   has_epoch = true;
   log_region_full = head_block_erased = head_block_passed_over = false;
   head_index = passed_over_count = 0;
   span_last_block = NO_BLOCK;
   clear_page_assembly();
   establish_window();
   return true;
}

static void nandlog_retrieve_epoch_details_locked(void *blob, uint16_t length)
{
   // Metadata lives in a ring, validated by its own CRC, rather than in the log itself
   memset(blob, 0, length);
   if (!is_initialized || (view_ring_page() == NO_PAGE))
      return;
   part_awake();
   for (uint32_t copy = 0; copy < 2; ++copy)
   {
      // Either copy of the slot, whichever still holds the details whole
      nandlog_chip_read_page(transfer_buffer, view_ring_page() ^ copy);
      const nandlog_meta_header_t *header = (const nandlog_meta_header_t*)transfer_buffer;
      const uint8_t *stored = transfer_buffer + sizeof(meta_slot_t);
      if (meta_header_valid(header) && (header->epoch == view_epoch()) && (header->details_crc == crc32_compute(stored, header->details_length)))
      {
         memcpy(blob, stored, (header->details_length < length) ? header->details_length : length);
         break;
      }
   }
   part_asleep();
}

static uint32_t record_prefix_length(void)
{
   // Plain C rather than a preprocessor branch: the condition is a compile-time constant, so this folds to a
   // literal, and the code reads as code
   return NANDLOG_RECORD_FRAMING ? NANDLOG_FRAMING_LENGTH_BYTES : 0;
}

static void write_record_prefix(uint8_t *destination, uint32_t data_length)
{
   // A record never exceeds a page, so a 16-bit length is always wide enough
   if (NANDLOG_RECORD_FRAMING)
   {
      const uint16_t length = (uint16_t)data_length;
      memcpy(destination, &length, sizeof(length));
   }
}

static nandlog_status_t refuse_record(nandlog_status_t reason)
{
   ++stats.records_refused;
   return reason;
}

static nandlog_status_t nandlog_store_record_locked(uint8_t record_type, uint32_t timestamp, const void *data, uint32_t data_length)
{
   // Add a complete record to the page being assembled, or say exactly why not
   if (!is_initialized)
      return refuse_record(NANDLOG_ERR_NOT_INITIALIZED);
   if (sealed)
      return refuse_record(NANDLOG_ERR_SEALED);
   if (disabled || unwritable)
      return refuse_record(NANDLOG_ERR_DISABLED);
   if (!epoch_is_current)
      return refuse_record(NANDLOG_ERR_EARLIER_EPOCH);
   if (!has_epoch)
      return refuse_record(NANDLOG_ERR_NO_EPOCH);

   // A single record larger than a page cannot be represented; refuse it rather than corrupt the stream. The
   // length is tested on its own first, so a huge one cannot wrap the sum around to something small
   const uint32_t prefix_length = record_prefix_length();
   if (data_length > (data_bytes_per_page - prefix_length - 1 - sizeof(timestamp)))
      return refuse_record(NANDLOG_ERR_TOO_LARGE);
   const uint32_t record_length = prefix_length + 1 + sizeof(timestamp) + data_length;

   // A full region has no page left to commit a record to, so nothing more is buffered for it
   if (log_region_full)
      return refuse_record(NANDLOG_ERR_FULL);

   // A record may carry an earlier time than one already in the page, as when it was stamped at the start of
   // something that finished after another writer's record went in. A step that small is kept exactly as
   // stamped, so records from different devices still match; only a larger one means the time base moved,
   // and commits the page
   const uint32_t bound = time_bound(timestamp);
   const bool time_moved_backwards = (page_last_timestamp != NANDLOG_NO_TIMESTAMP) && (bound < page_last_timestamp) && ((page_last_timestamp - bound) > NANDLOG_TIMESTAMP_TOLERANCE_MS);

   // Records are never split across pages, so a record that does not fit in what remains of the current
   // page commits that page and starts the next one
   bool committed = false;
   if (cache_index && (((cache_index + record_length) > data_bytes_per_page) || time_moved_backwards))
   {
      // Writing a page is impossible while a download is in progress
      if (is_reading)
         return refuse_record(NANDLOG_ERR_READING);
      const nandlog_status_t status = commit_current_page();
      if (status != NANDLOG_OK)
         return refuse_record(status);
      if (log_region_full)
         return refuse_record(NANDLOG_ERR_FULL);
      committed = true;
   }

   uint32_t offset = cache_index;
   write_record_prefix(cache + offset, data_length);
   offset += prefix_length;
   memcpy(cache + offset, &record_type, sizeof(record_type));
   memcpy(cache + offset + sizeof(record_type), &timestamp, sizeof(timestamp));
   if (data_length)
      memcpy(cache + offset + sizeof(record_type) + sizeof(timestamp), data, data_length);
   cache_crc = crc32_continue(cache_crc, cache + cache_index, record_length);          // the page's running CRC
   cache_index += record_length;

   // Track the smallest and largest times in the page and its record count, which its header will advertise
   if ((page_first_timestamp == NANDLOG_NO_TIMESTAMP) || (bound < page_first_timestamp))
      page_first_timestamp = bound;
   if ((page_last_timestamp == NANDLOG_NO_TIMESTAMP) || (bound > page_last_timestamp))
      page_last_timestamp = bound;
   ++page_record_count;
   ++stats.records_stored;

   // A call that committed nothing may erase the window's next block. If that leaves the buffered records nowhere to
   // go, this record is refused and the others are counted lost
   const uint32_t lost = stats.records_lost;
   if (!committed && erase_ahead_due())
      erase_ahead_once();
   if (stats.records_lost == lost)
      return NANDLOG_OK;
   --stats.records_stored;
   --stats.records_lost;
   return refuse_record(unwritable ? NANDLOG_ERR_UNWRITABLE : NANDLOG_ERR_FULL);
}

static nandlog_status_t nandlog_flush_locked(bool write_partial_pages)
{
   // Commit what is buffered if the page is full or a partial page is asked for. Records taken before
   // nandlog_disable(true) can still be committed, since disabling stops the log accepting records, not keeping them
   if (!is_initialized)
      return NANDLOG_ERR_NOT_INITIALIZED;
   if (sealed)
      return NANDLOG_ERR_SEALED;
   if (unwritable)
      return NANDLOG_ERR_DISABLED;
   if (!epoch_is_current)
      return NANDLOG_ERR_EARLIER_EPOCH;
   if (is_reading)
      return NANDLOG_ERR_READING;
   if (cache_index && (write_partial_pages || (cache_index >= data_bytes_per_page)))
      return commit_current_page();

   // Nothing was committed, so this call may erase the window's next block. Report it if that leaves the buffered
   // records nowhere to go
   const uint32_t lost = stats.records_lost;
   if (erase_ahead_due())
      erase_ahead_once();
   if (stats.records_lost == lost)
      return NANDLOG_OK;
   return unwritable ? NANDLOG_ERR_UNWRITABLE : NANDLOG_ERR_FULL;
}

static void nandlog_begin_reading_locked(uint32_t starting_timestamp, uint32_t ending_timestamp)
{
   // Establish the span to be read, in positions of the view's epoch. Buffered records are committed first, so that a
   // page served under a sequence number never changes
   is_reading = false;
   if (!is_initialized || !in_session)
      return;
   if (epoch_is_current && has_epoch && cache_index && !unwritable && !log_region_full)
      commit_current_page();
   if (epoch_is_current && layout_unverified)
      NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: Epoch %u is read through a bad-block table that no longer lays it out as it was written; pages may be missing\n", log_epoch);
   const uint32_t total = view_positions();
   uint32_t first = 0, end = total;
   if (starting_timestamp)
      first = seek_position_for_timestamp(starting_timestamp, true);
   if (ending_timestamp && (first < total))
   {
      // A window that ends before the first page found for its start selects nothing, rather than wrapping all the
      // way around the region from one to the other
      const uint32_t last = seek_position_for_timestamp(ending_timestamp, false);
      if ((last >= total) || (last < first))
         first = total;
      else
         end = last + 1;
   }
   if (first >= total)
      first = end = total;
   reading_index = first;
   reading_end = end;
   reading_seq = first;
   reading_page = (first < total) ? view_page(first) : NO_PAGE;
   is_reading = true;
}

static void nandlog_end_reading_locked(void)
{
   is_reading = false;
}

static void nandlog_begin_session_locked(void)
{
   if (!in_session)
   {
      nandlog_port_power(true);
      nandlog_chip_low_power(false);
   }
   in_session = true;
}

static void nandlog_end_session_locked(void)
{
   nandlog_end_reading_locked();
   epoch_is_current = true;
   if (in_session)
   {
      nandlog_chip_low_power(true);
      nandlog_port_power(false);
   }
   in_session = false;
}

static void nandlog_read_span_locked(uint32_t *num_pages, uint32_t *num_bytes)
{
   // The page count is arithmetic, but an exact byte total is not: it costs a read of every page in the span,
   // which doubles the flash traffic of a download. That pass is skipped entirely when the caller passes NULL
   uint32_t pages = 0, bytes = 0;
   if (is_reading && (reading_index < reading_end))
   {
      pages = reading_end - reading_index;
      if (num_bytes)
      {
         uint32_t page = reading_page;
         for (uint32_t position = reading_index; position < reading_end; ++position)
         {
            nandlog_page_header_t stored;
            bool known;
            bytes += read_view_page(page, NULL, &stored, &known);
            page = next_good_page(page);
         }
      }
   }
   if (num_pages)
      *num_pages = pages;
   if (num_bytes)
      *num_bytes = bytes;
}

static uint32_t nandlog_retrieve_next_page_locked(uint8_t *buffer, nandlog_page_header_t *header)
{
   // Retrieve the next page, optionally with the metadata an offload stream needs to frame it
   nandlog_page_header_t discarded;
   if (!header)
      header = &discarded;
   memset(header, 0, sizeof(*header));
   header->magic = NANDLOG_PAGE_MAGIC_THIS_BUILD;
   header->epoch = view_epoch();
   header->first_timestamp = header->last_timestamp = NANDLOG_NO_TIMESTAMP;
   if (!is_reading || (reading_index >= reading_end))
   {
      is_reading = false;
      return 0;
   }

   // A page whose header cannot be trusted is identified by the sequence number it must have carried. A trusted one
   // keeps its own magic, which need not be this build's
   nandlog_page_header_t stored;
   bool known;
   const uint32_t length = read_view_page(reading_page, buffer, &stored, &known);
   if (known)
   {
      header->magic = stored.magic;
      header->seq = stored.seq;
      header->first_timestamp = stored.first_timestamp;
      header->last_timestamp = stored.last_timestamp;
      header->record_count = stored.record_count;
      reading_seq = stored.seq + 1;
   }
   else
      header->seq = reading_seq++;
   reading_page = next_good_page(reading_page);

   // The payload CRC was already checked over exactly the bytes served, so it is reported without computing it again
   header->payload_length = (uint16_t)length;
   header->payload_crc = length ? stored.payload_crc : 0;
   if (length)
      check_crc("A page served", header->payload_crc, buffer, length);
   if (++reading_index >= reading_end)
      is_reading = false;
   return length;
}

static uint32_t fetch_page_by_seq(uint32_t seq, uint8_t *buffer, nandlog_page_header_t *header)
{
   // One page of the view by its sequence number, or a gap
   memset(header, 0, sizeof(*header));
   header->magic = NANDLOG_PAGE_MAGIC_THIS_BUILD;
   header->epoch = view_epoch();
   header->seq = seq;
   header->first_timestamp = header->last_timestamp = NANDLOG_NO_TIMESTAMP;
   uint32_t position = 0;
   if (!find_position_by_seq(seq, &position))
      return 0;
   nandlog_page_header_t stored;
   bool known;
   const uint32_t length = read_view_page(view_page(position), buffer, &stored, &known);
   if (!length || (stored.seq != seq))
      return 0;                        // failed validation again; still a gap
   *header = stored;
   return length;
}

static uint32_t nandlog_retrieve_page_by_seq_locked(uint32_t seq, uint8_t *buffer, nandlog_page_header_t *header)
{
   // Fetch one specific page so the host can ask for the ones it lost. No read need be open, so a host can ask
   // after a read has run to its end
   if (!is_initialized)
   {
      memset(header, 0, sizeof(*header));
      header->seq = seq;
      return 0;
   }
   part_awake();
   const uint32_t length = fetch_page_by_seq(seq, buffer, header);
   part_asleep();
   return length;
}

static uint32_t nandlog_read_recent_page_locked(uint32_t pages_back, uint8_t *buffer, nandlog_page_header_t *header, bool *end_of_epoch)
{
   // Walk backwards from the write head, newest page first, deliberately record-agnostic
   // Returns the payload length or zero if that page could not be read
   if (end_of_epoch)
      *end_of_epoch = true;
   const uint32_t positions = is_initialized ? view_positions() : 0;
   if (pages_back >= positions)
      return 0;
   const uint32_t position = positions - 1 - pages_back;
   if (end_of_epoch)
      *end_of_epoch = (position == 0);

   part_awake();
   nandlog_page_header_t stored;
   bool known;
   const uint32_t length = read_view_page(view_page(position), buffer, &stored, &known);
   if (known && header)
      memcpy(header, &stored, sizeof(*header));
   part_asleep();
   return length;
}

static uint32_t nandlog_epoch_count_locked(void)
{
   uint32_t epochs[NANDLOG_MAX_LISTED_EPOCHS], ring_pages[NANDLOG_MAX_LISTED_EPOCHS];
   if (!is_initialized)
      return 0;
   part_awake();
   const uint32_t count = list_epochs(epochs, ring_pages, NANDLOG_MAX_LISTED_EPOCHS);
   part_asleep();
   return count;
}

static bool nandlog_epoch_info_locked(uint32_t index, nandlog_epoch_info_t *info)
{
   uint32_t epochs[NANDLOG_MAX_LISTED_EPOCHS], ring_pages[NANDLOG_MAX_LISTED_EPOCHS];
   memset(info, 0, sizeof(*info));
   if (!is_initialized || (index >= NANDLOG_MAX_LISTED_EPOCHS))
      return false;
   part_awake();
   bool described = false;
   meta_slot_t slot;
   if ((index < list_epochs(epochs, ring_pages, index + 1)) && read_slot(ring_pages[index], &slot))
   {
      info->epoch = slot.header.epoch;
      info->created_timestamp = slot.header.created_timestamp;
      info->first_page = slot.header.log_start_page;
      info->details_length = slot.header.details_length;
      info->is_current = (slot.header.epoch == log_epoch);
      info->is_sealed = info->is_current && sealed;
      info->layout_unverified = info->is_current && layout_unverified;
      described = true;
   }
   part_asleep();
   return described;
}

static void nandlog_select_current_epoch_locked(void)
{
   // A read open over the other view ends with it
   if (!epoch_is_current)
      is_reading = false;
   epoch_is_current = true;
}

static bool nandlog_select_epoch_locked(uint32_t epoch)
{
   // A selection is part of a read
   if (!is_initialized || !in_session || is_reading)
      return false;
   if (has_epoch && (epoch == log_epoch))
   {
      nandlog_select_current_epoch_locked();
      return true;
   }

   // An epoch ends where the one that replaced it began, so the bound comes from the next newer epoch listed
   uint32_t epochs[NANDLOG_MAX_LISTED_EPOCHS], ring_pages[NANDLOG_MAX_LISTED_EPOCHS];
   const uint32_t count = list_epochs(epochs, ring_pages, NANDLOG_MAX_LISTED_EPOCHS);
   uint32_t index = count;
   for (uint32_t i = 0; i < count; ++i)
      if (epochs[i] == epoch)
         index = i;
   meta_slot_t successor, selected;
   if ((index >= count) || !index || !read_slot(ring_pages[index - 1], &successor) || !read_slot(ring_pages[index], &selected))
      return false;

   selected_epoch = epoch;
   selected_start_page = selected.header.log_start_page;
   selected_ring_page = ring_pages[index];
   selected_positions = earlier_epoch_positions(epoch, selected.header.log_start_page, successor.header.log_start_page);
   epoch_is_current = false;
   return true;
}

static uint32_t nandlog_retransmit_add_locked(const uint32_t *seqs, uint32_t count)
{
   // Accumulate across commands: one offloading write cannot name every page a long transfer might have lost
   for (uint32_t i = 0; (i < count) && (retransmit_num_pages < NANDLOG_MAX_RETRANSMIT_PAGES); ++i)
      retransmit_seqs[retransmit_num_pages++] = seqs[i];
   return retransmit_num_pages;
}

static uint32_t nandlog_retransmit_total_bytes_locked(void)
{
   // The host sizes its receive buffer from the stream header, so this total has to be exact rather than an upper bound
   uint32_t total = 0;
   if (!is_initialized)
      return 0;
   part_awake();
   for (uint32_t i = 0; i < retransmit_num_pages; ++i)
   {
      nandlog_page_header_t header;
      total += fetch_page_by_seq(retransmit_seqs[i], NULL, &header);
   }
   part_asleep();
   return total;
}

static uint32_t nandlog_retrieve_retransmit_page_locked(uint32_t index, uint8_t *buffer, nandlog_page_header_t *header)
{
   // Answer the index-th requested page. A page that is still unreadable comes back as a zero-length frame
   // carrying the requested sequence number, so the host can tell "still missing" from "never answered"
   if (index >= retransmit_num_pages)
   {
      memset(header, 0, sizeof(*header));
      header->magic = NANDLOG_PAGE_MAGIC_THIS_BUILD;
      header->epoch = view_epoch();
      header->first_timestamp = header->last_timestamp = NANDLOG_NO_TIMESTAMP;
      return 0;
   }
   return nandlog_retrieve_page_by_seq_locked(retransmit_seqs[index], buffer, header);
}


// Public API Functions ------------------------------------------------------------------------------------------------

bool nandlog_probe(void)
{
   nandlog_port_lock();
   const bool result = nandlog_probe_locked();
   nandlog_port_unlock();
   return result;
}

bool nandlog_init(void)
{
   nandlog_port_lock();
   const bool result = nandlog_init_locked();
   nandlog_port_unlock();
   return result;
}

void nandlog_deinit(void)
{
   nandlog_port_lock();
   nandlog_deinit_locked();
   nandlog_port_unlock();
}

void nandlog_disable(bool disable)
{
   nandlog_port_lock();
   disabled = disable;
   nandlog_port_unlock();
}

uint32_t nandlog_data_bytes_per_page(void)
{
   // Answered from the chip rather than the cached copy so it is correct before nandlog_init() has run. The
   // geometry is constant, so there is nothing here to lock
   return nandlog_chip_geometry()->page_size_bytes - sizeof(nandlog_page_header_t);
}

bool nandlog_begin_epoch(const void *blob, uint16_t length)
{
   nandlog_port_lock();
   const bool result = nandlog_begin_epoch_locked(blob, length);
   nandlog_port_unlock();
   return result;
}

void nandlog_retrieve_epoch_details(void *blob, uint16_t length)
{
   nandlog_port_lock();
   nandlog_retrieve_epoch_details_locked(blob, length);
   nandlog_port_unlock();
}

nandlog_status_t nandlog_store_record(uint8_t record_type, uint32_t timestamp, const void *data, uint32_t data_length)
{
   nandlog_port_lock();
   const nandlog_status_t result = nandlog_store_record_locked(record_type, timestamp, data, data_length);
   nandlog_port_unlock();
   return result;
}

nandlog_status_t nandlog_flush(bool write_partial_pages)
{
   nandlog_port_lock();
   const nandlog_status_t result = nandlog_flush_locked(write_partial_pages);
   nandlog_port_unlock();
   return result;
}

bool nandlog_has_buffered_data(void)
{
   // Reports whether any collected data is sitting unwritten in RAM
   nandlog_port_lock();
   const bool result = (cache_index != 0);
   nandlog_port_unlock();
   return result;
}

void nandlog_begin_session(void)
{
   nandlog_port_lock();
   nandlog_begin_session_locked();
   nandlog_port_unlock();
}

void nandlog_end_session(void)
{
   nandlog_port_lock();
   nandlog_end_session_locked();
   nandlog_port_unlock();
}

void nandlog_begin_reading(uint32_t starting_timestamp, uint32_t ending_timestamp)
{
   nandlog_port_lock();
   nandlog_begin_reading_locked(starting_timestamp, ending_timestamp);
   nandlog_port_unlock();
}

void nandlog_end_reading(void)
{
   nandlog_port_lock();
   nandlog_end_reading_locked();
   nandlog_port_unlock();
}

void nandlog_read_span(uint32_t *num_pages, uint32_t *num_bytes)
{
   nandlog_port_lock();
   nandlog_read_span_locked(num_pages, num_bytes);
   nandlog_port_unlock();
}

uint32_t nandlog_retrieve_next_page(uint8_t *buffer, nandlog_page_header_t *header)
{
   nandlog_port_lock();
   const uint32_t result = nandlog_retrieve_next_page_locked(buffer, header);
   nandlog_port_unlock();
   return result;
}

uint32_t nandlog_retrieve_page_by_seq(uint32_t seq, uint8_t *buffer, nandlog_page_header_t *header)
{
   nandlog_port_lock();
   const uint32_t result = nandlog_retrieve_page_by_seq_locked(seq, buffer, header);
   nandlog_port_unlock();
   return result;
}

bool nandlog_framed_next_record(const uint8_t *payload, uint32_t length, uint32_t *offset, const uint8_t **record, uint32_t *record_bytes)
{
   if (!payload || !offset || (*offset >= length))
      return false;
   uint16_t data_length = 0;
   if ((*offset + NANDLOG_FRAMING_LENGTH_BYTES) > length)
      return false;
   memcpy(&data_length, payload + *offset, sizeof(data_length));
   const uint32_t body = *offset + NANDLOG_FRAMING_LENGTH_BYTES;
   const uint32_t bytes = 5 + (uint32_t)data_length;
   if ((body + bytes) > length)
      return false;
   if (record)
      *record = payload + body;
   if (record_bytes)
      *record_bytes = bytes;
   *offset = body + bytes;
   return true;
}

uint32_t nandlog_read_recent_page(uint32_t pages_back, uint8_t *buffer, nandlog_page_header_t *header, bool *end_of_epoch)
{
   nandlog_port_lock();
   const uint32_t result = nandlog_read_recent_page_locked(pages_back, buffer, header, end_of_epoch);
   nandlog_port_unlock();
   return result;
}

uint32_t nandlog_epoch_count(void)
{
   nandlog_port_lock();
   const uint32_t result = nandlog_epoch_count_locked();
   nandlog_port_unlock();
   return result;
}

bool nandlog_epoch_info(uint32_t index, nandlog_epoch_info_t *info)
{
   nandlog_port_lock();
   const bool result = nandlog_epoch_info_locked(index, info);
   nandlog_port_unlock();
   return result;
}

bool nandlog_select_epoch(uint32_t epoch)
{
   nandlog_port_lock();
   const bool result = nandlog_select_epoch_locked(epoch);
   nandlog_port_unlock();
   return result;
}

void nandlog_select_current_epoch(void)
{
   nandlog_port_lock();
   nandlog_select_current_epoch_locked();
   nandlog_port_unlock();
}

void nandlog_retransmit_clear(void)
{
   nandlog_port_lock();
   retransmit_num_pages = 0;
   nandlog_port_unlock();
}

uint32_t nandlog_retransmit_add(const uint32_t *seqs, uint32_t count)
{
   nandlog_port_lock();
   const uint32_t result = nandlog_retransmit_add_locked(seqs, count);
   nandlog_port_unlock();
   return result;
}

uint32_t nandlog_retransmit_count(void)
{
   nandlog_port_lock();
   const uint32_t result = retransmit_num_pages;
   nandlog_port_unlock();
   return result;
}

uint32_t nandlog_retransmit_total_bytes(void)
{
   nandlog_port_lock();
   const uint32_t result = nandlog_retransmit_total_bytes_locked();
   nandlog_port_unlock();
   return result;
}

uint32_t nandlog_retrieve_retransmit_page(uint32_t index, uint8_t *buffer, nandlog_page_header_t *header)
{
   nandlog_port_lock();
   const uint32_t result = nandlog_retrieve_retransmit_page_locked(index, buffer, header);
   nandlog_port_unlock();
   return result;
}

uint32_t nandlog_bad_block_count(void)
{
   nandlog_port_lock();
   const uint32_t result = bad_block_count;
   nandlog_port_unlock();
   return result;
}

void nandlog_get_stats(nandlog_stats_t *out)
{
   nandlog_port_lock();
   *out = stats;
   nandlog_port_unlock();
}

void nandlog_reset_bad_block_table(void)
{
   nandlog_port_lock();
   nandlog_reset_bad_block_table_locked();
   nandlog_port_unlock();
}


#else

bool nandlog_probe(void) { return true; }
bool nandlog_init(void) { return true; }
void nandlog_deinit(void) {}
void nandlog_disable(bool disable) { (void)disable; }
uint32_t nandlog_data_bytes_per_page(void) { return NANDLOG_MAX_DATA_BYTES_PER_PAGE; }
bool nandlog_begin_epoch(const void *blob, uint16_t length) { (void)blob; (void)length; return true; }
void nandlog_retrieve_epoch_details(void *blob, uint16_t length) { memset(blob, 0, length); }
nandlog_status_t nandlog_store_record(uint8_t record_type, uint32_t timestamp, const void *data, uint32_t data_length) { (void)record_type; (void)timestamp; (void)data; (void)data_length; return NANDLOG_OK; }
nandlog_status_t nandlog_flush(bool write_partial_pages) { (void)write_partial_pages; return NANDLOG_OK; }
bool nandlog_has_buffered_data(void) { return false; }
void nandlog_begin_reading(uint32_t starting_timestamp, uint32_t ending_timestamp) { (void)starting_timestamp; (void)ending_timestamp; }
void nandlog_end_reading(void) {}
void nandlog_begin_session(void) {}
void nandlog_end_session(void) {}
void nandlog_read_span(uint32_t *num_pages, uint32_t *num_bytes) { if (num_pages) *num_pages = 0; if (num_bytes) *num_bytes = 0; }
uint32_t nandlog_retrieve_next_page(uint8_t *buffer, nandlog_page_header_t *header) { (void)buffer; (void)header; return 0; }
uint32_t nandlog_retrieve_page_by_seq(uint32_t seq, uint8_t *buffer, nandlog_page_header_t *header) { (void)seq; (void)buffer; (void)header; return 0; }
bool nandlog_framed_next_record(const uint8_t *payload, uint32_t length, uint32_t *offset, const uint8_t **record, uint32_t *record_bytes) { (void)payload; (void)length; (void)offset; (void)record; (void)record_bytes; return false; }
uint32_t nandlog_read_recent_page(uint32_t pages_back, uint8_t *buffer, nandlog_page_header_t *header, bool *end_of_epoch) { (void)pages_back; (void)buffer; (void)header; if (end_of_epoch) *end_of_epoch = true; return 0; }
uint32_t nandlog_epoch_count(void) { return 0; }
bool nandlog_epoch_info(uint32_t index, nandlog_epoch_info_t *info) { (void)index; memset(info, 0, sizeof(*info)); return false; }
bool nandlog_select_epoch(uint32_t epoch) { (void)epoch; return false; }
void nandlog_select_current_epoch(void) {}
void nandlog_retransmit_clear(void) {}
uint32_t nandlog_retransmit_add(const uint32_t *seqs, uint32_t count) { (void)seqs; (void)count; return 0; }
uint32_t nandlog_retransmit_count(void) { return 0; }
uint32_t nandlog_retransmit_total_bytes(void) { return 0; }
uint32_t nandlog_retrieve_retransmit_page(uint32_t index, uint8_t *buffer, nandlog_page_header_t *header) { (void)index; (void)buffer; (void)header; return 0; }
uint32_t nandlog_bad_block_count(void) { return 0; }
void nandlog_get_stats(nandlog_stats_t *stats) { memset(stats, 0, sizeof(*stats)); }
void nandlog_reset_bad_block_table(void) {}

#endif  // #if NANDLOG_HAS_HARDWARE
