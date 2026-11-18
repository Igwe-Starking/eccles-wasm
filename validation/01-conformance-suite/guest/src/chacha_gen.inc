    ew_println("-- ChaCha20 block, RFC 8439 section 2.3.2 test vector");
    { static const uint32_t exp[16] = {0xe4e7f110u,0x15593bd1u,0x1fdd0f50u,0xc47120a3u,0xc7f4d1c7u,0x0368c033u,0x9aaa2204u,0x4e6cd4c3u,0x466482d2u,0x09aa9f07u,0x05d7c214u,0xa2028bd9u,0xd19c12b5u,0xb94e16deu,0xe883d0cbu,0x4e3c50a2u};
      uint32_t blk[16]; int i; chacha20_block(blk);
      for (i = 0; i < 16; i++) { char nm[16]; nm[0]='w'; nm[1]='o'; nm[2]='r'; nm[3]='d'; nm[4]=' '; nm[5]=(char)('0'+i/10); nm[6]=(char)('0'+i%10); nm[7]=0; chk_u32(nm, blk[i], exp[i]); } }
