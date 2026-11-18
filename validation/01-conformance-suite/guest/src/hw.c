/* hw.c - ECCLES WASM test module 1/4: hardware + timing + system.
 * Optional jumper wires (without them those checks report INFO, never FAIL):
 *   GPIO 26 <-> GPIO 27   PWM / interrupt / GPIO loopback
 *   GPIO 17 <-> GPIO 16   UART2 loopback (TX2 <-> RX2)
 *   GPIO 23 <-> GPIO 19   SPI loopback (MOSI <-> MISO), SCK = GPIO 18
 *   I2C bus: SDA = GPIO 21, SCL = GPIO 22 (any attached device is listed)
 */
#include "common.h"
/* =========================================================================================
 * t_info
 * ========================================================================================= */
EW_EXPORT("t_info") void t_info(void) {
    head("INFO");
    ew_println("ECCLES WASM hardware + compute test");
    ew_printf("guest linear memory: %u pages\n", (unsigned)WASM_MEM_SIZE());
    ew_printf("uptime at start: %u ms\n", (unsigned)ew_millis());
    rep(K_UNSUP, "i2s", "no I2S import exists in the runtime API (env imports: gpio/analog/uart/i2c/spi only)");
    ew_println("legend: PASS ok | FAIL wrong result | INFO needs hardware/jumper | UNSUP chip/backend can't do it | SKIP not run");
}

/* =========================================================================================
 * t_time : millis / micros / sleep accuracy / call overhead
 * ========================================================================================= */
EW_EXPORT("t_time") void t_time(void) {
    uint32_t m0 = 0, u0 = 0, m1 = 0, u1 = 0, a, b, d, mn, steps, i; int s1, s2;
    head("TIME (millis / micros / sleep)");
    s1 = ew_millis_(&m0); s2 = ew_micros_(&u0);
    rep_st("millis()", s1, ""); rep_st("micros()", s2, "");
    rep(K_INFO, "readings", "millis=%u micros=%u", (unsigned)m0, (unsigned)u0);
    { uint32_t um = u0 / 1000u; int32_t df = (int32_t)(um - m0);
      rep(df >= -2 && df <= 2 ? K_PASS : K_FAIL, "millis vs micros/1000", "diff=%d ms", (int)df); }

    a = ew_micros(); ew_sleep(100); b = ew_micros(); d = b - a;
    rep(d >= 89000u && d <= 125000u ? K_PASS : K_FAIL, "sleep(100 ms)", "measured %u us (RTOS tick can round down by up to 1 tick)", (unsigned)d);
    a = ew_micros(); ew_sleep(1); b = ew_micros(); d = b - a;
    rep(K_INFO, "sleep(1 ms)", "measured %u us (RTOS tick granularity)", (unsigned)d);
    a = ew_micros(); ew_sleep_us(500); b = ew_micros(); d = b - a;
    rep(d >= 500u && d <= 3000u ? K_PASS : K_FAIL, "sleep_us(500)", "measured %u us", (unsigned)d);
    a = ew_micros(); ew_sleep_us(100); b = ew_micros(); d = b - a;
    rep(d >= 100u && d <= 1500u ? K_PASS : K_FAIL, "sleep_us(100)", "measured %u us", (unsigned)d);
    a = ew_micros(); ew_sleep_us(5000); b = ew_micros(); d = b - a;
    rep(d >= 4900u && d <= 25000u ? K_PASS : K_FAIL, "sleep_us(5000)", "measured %u us", (unsigned)d);

    /* host-call overhead: 2000 back-to-back micros() reads */
    TIC(); for (i = 0; i < 2000; i++) (void)ew_micros(); d = TOC();
    rep(K_INFO, "host call overhead", "ew_micros() x2000 = %u us -> %u ns per call (guest->host->guest)", (unsigned)d, (unsigned)(d * 1000u / 2000u));

    /* timer resolution: smallest non-zero step between consecutive reads */
    mn = 0xFFFFFFFFu; steps = 0; a = ew_micros();
    for (i = 0; i < 400; i++) { b = ew_micros(); if (b != a) { if (b - a < mn) mn = b - a; steps++; a = b; } }
    rep(K_INFO, "micros resolution", "smallest step %u us, %u distinct steps in 400 reads", (unsigned)(steps ? mn : 0), (unsigned)steps);

    /* monotonic */
    { uint32_t prev = ew_micros(), cur; int bad = 0;
      for (i = 0; i < 500; i++) { cur = ew_micros(); if ((int32_t)(cur - prev) < 0) bad++; prev = cur; }
      rep(bad == 0 ? K_PASS : K_FAIL, "micros monotonic", "%d backwards steps in 500 reads", bad); }

    ew_millis_(&m1); ew_micros_(&u1);
    rep(K_INFO, "elapsed this section", "%u ms", (unsigned)(m1 - m0));
    rep(K_INFO, "wrap", "millis wraps at 49.7 days, micros every 71.6 minutes (use unsigned subtraction)");
}

/* =========================================================================================
 * t_system : wdt / yield / log / random / memory throughput / reset+sleep skipped
 * ========================================================================================= */
static uint8_t membuf[16384];
EW_EXPORT("t_system") void t_system(void) {
    uint32_t v, i, t, a; uint32_t rnd[64]; int distinct = 0, j; uint32_t ones = 0; int st;
    head("SYSTEM (wdt / yield / random / memory)");
    rep_st("wdt_feed()", ew_wdt_feed(), "");
    rep_st("yield()", ew_yield(), "");
    st = ew_log("log-call-ok\n", 12); rep_st("log()", st, "(line above is the payload)");

    for (i = 0; i < 64; i++) { st = ew_random_(&v); rnd[i] = v; }
    rep_st("random()", st, "");
    for (i = 0; i < 64; i++) { int dup = 0; for (j = 0; (uint32_t)j < i; j++) if (rnd[j] == rnd[i]) dup = 1; if (!dup) distinct++; }
    for (i = 0; i < 64; i++) ones += (uint32_t)__builtin_popcount(rnd[i]);
    rep(distinct >= 63 ? K_PASS : K_FAIL, "random distinct", "%d of 64 unique", distinct);
    rep(ones > 900u && ones < 1150u ? K_PASS : K_FAIL, "random bit balance", "%u of 2048 bits set (expect ~1024)", (unsigned)ones);
    rep(K_INFO, "random sample", "0x%08X 0x%08X 0x%08X", (unsigned)rnd[0], (unsigned)rnd[1], (unsigned)rnd[2]);

    /* linear-memory fill + verify + copy throughput on 16 KB */
    TIC(); for (i = 0; i < sizeof membuf; i++) membuf[i] = (uint8_t)(i * 7u + 3u); t = TOC();
    rep(K_INFO, "memory fill 16 KB", "%u us (%u KB/s)", (unsigned)t, (unsigned)(t ? 16u * 1000000u / t : 0));
    a = 0; TIC(); for (i = 0; i < sizeof membuf; i++) if (membuf[i] != (uint8_t)(i * 7u + 3u)) a++; t = TOC();
    rep(a == 0 ? K_PASS : K_FAIL, "memory verify 16 KB", "%u us, %u errors", (unsigned)t, (unsigned)a);
    TIC(); memcpy(membuf + 8192, membuf, 8192); t = TOC();
    rep(memcmp(membuf + 8192, membuf, 8192) == 0 ? K_PASS : K_FAIL, "memcpy 8 KB", "%u us (%u KB/s)", (unsigned)t, (unsigned)(t ? 8u * 1000000u / t : 0));

    rep(K_SKIP, "reset()", "not run: it would reboot the chip and end this test");
    rep(K_SKIP, "deep_sleep()", "not run: it would power down and end this test");
}

/* =========================================================================================
 * GPIO
 * ========================================================================================= */
static void gpio_pin(int pin) {
    uint32_t hi = 9, lo = 9, tg = 9, pu = 9, pd = 9, fl = 9; int s_hi, s_lo, s_tg, s_m;
    s_m = ew_pin_mode(pin, EW_OUTPUT);
    if (s_m != EW_OK) { rep(K_FAIL, "gpio", "pin %d: mode(OUTPUT) -> %s", pin, ew_status_name(s_m)); return; }
    s_hi = ew_write(pin, 1); ew_sleep_us(30); ew_read(pin, &hi);
    s_lo = ew_write(pin, 0); ew_sleep_us(30); ew_read(pin, &lo);
    s_tg = ew_toggle(pin);   ew_sleep_us(30); ew_read(pin, &tg);   /* low -> high */
    ew_write(pin, 0);
    {
        int ok = (s_hi == EW_OK && s_lo == EW_OK && s_tg == EW_OK && hi == 1 && lo == 0 && tg == 1);
        ew_pin_mode(pin, EW_INPUT_PULLUP);   ew_sleep_us(200); ew_read(pin, &pu);
        ew_pin_mode(pin, EW_INPUT_PULLDOWN); ew_sleep_us(200); ew_read(pin, &pd);
        ew_pin_mode(pin, EW_INPUT);          ew_sleep_us(50);  ew_read(pin, &fl);
        rep(ok ? K_PASS : K_FAIL, "gpio", "pin %2d: out hi=%u lo=%u toggle=%u | pullup=%u pulldown=%u float=%u %s", pin,
            (unsigned)hi, (unsigned)lo, (unsigned)tg, (unsigned)pu, (unsigned)pd, (unsigned)fl,
            (pu == 1 && pd == 0) ? "(pulls ok)" : "(pin loaded/driven externally)");
    }
}
EW_EXPORT("t_gpio") void t_gpio(void) {
    static const uint8_t outs[] = { 2, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33 };
    static const uint8_t ins[] = { 34, 35, 36, 39 };
    unsigned i; uint32_t lv; int st;
    head("GPIO (outputs: write/read-back/toggle + pull-up/pull-down)");
    ew_println("skipped on purpose: 0 (boot), 1/3 (console UART0), 6-11 (flash)");
    for (i = 0; i < sizeof outs; i++) { gpio_pin(outs[i]); ew_sleep(2); }
    for (i = 0; i < sizeof ins; i++) {
        st = ew_pin_mode(ins[i], EW_INPUT); lv = 9; if (st == EW_OK) st = ew_read(ins[i], &lv);
        rep(st == EW_OK ? K_PASS : K_FAIL, "gpio", "pin %2d: input-only, level=%u (no internal pulls on 34-39)", ins[i], (unsigned)lv);
    }
    rep_soft("gpio invalid pin 99 mode", ew_pin_mode(99, EW_OUTPUT));
    rep_soft("gpio invalid pin 99 read", ew_read(99, &lv));
}

/* =========================================================================================
 * PWM + interrupts (+ GPIO loopback) on 26 -> 27
 * ========================================================================================= */
EW_EXPORT("t_pwm_irq") void t_pwm_irq(void) {
    uint32_t lv = 0, cnt = 0, i; int st, jumper;
    head("PWM + INTERRUPTS (loopback GPIO26 -> GPIO27)");
    ew_pin_mode(27, EW_INPUT_PULLDOWN); ew_pin_mode(26, EW_OUTPUT);
    ew_write(26, 1); ew_sleep_us(200); ew_read(27, &lv); jumper = (lv == 1);
    ew_write(26, 0); ew_sleep_us(200); ew_read(27, &lv); jumper = jumper && (lv == 0);
    rep(jumper ? K_PASS : K_INFO, "loopback 26<->27", jumper ? "jumper detected" : "no jumper (checks below that need it report INFO)");

    st = ew_irq_attach(27, EW_RISING); rep_st("irq_attach(27, RISING)", st, "");
    ew_irq_clear(27);
    for (i = 0; i < 50; i++) { ew_write(26, 1); ew_write(26, 0); }
    st = ew_irq_count(27, &cnt); rep_st("irq_count()", st, "");
    if (jumper) rep(cnt == 50u ? K_PASS : K_FAIL, "gpio-driven edges", "%u rising edges counted (expect 50)", (unsigned)cnt);
    else        rep(K_INFO, "gpio-driven edges", "%u counted (needs jumper 26<->27 for 50)", (unsigned)cnt);
    rep_st("irq_clear()", ew_irq_clear(27), "");

    st = ew_pwm(26, 1000, 32768); rep_st("pwm(26, 1 kHz, 50%)", st, "");
    ew_irq_clear(27); ew_sleep(200); ew_irq_count(27, &cnt);
    if (jumper && st == EW_OK) rep((cnt >= 170u && cnt <= 230u) ? K_PASS : K_FAIL, "pwm 1 kHz frequency", "%u edges in 200 ms (expect ~200)", (unsigned)cnt);
    else                       rep(K_INFO, "pwm 1 kHz frequency", "%u edges in 200 ms (needs jumper 26<->27)", (unsigned)cnt);
    st = ew_pwm(26, 5000, 16384); rep_st("pwm(26, 5 kHz, 25%)", st, "");
    ew_irq_clear(27); ew_sleep(100); ew_irq_count(27, &cnt);
    if (jumper && st == EW_OK) rep((cnt >= 450u && cnt <= 550u) ? K_PASS : K_FAIL, "pwm 5 kHz frequency", "%u edges in 100 ms (expect ~500)", (unsigned)cnt);
    else                       rep(K_INFO, "pwm 5 kHz frequency", "%u edges in 100 ms", (unsigned)cnt);
    rep_st("pwm duty 0%", ew_pwm(26, 5000, 0), "");
    rep_st("pwm duty 100%", ew_pwm(26, 5000, 65535), "");
    rep_st("analog(26, 128) (8-bit duty)", ew_analog(26, 128), "");
    rep_st("analog(26, 0)", ew_analog(26, 0), "");
    rep_bad("pwm duty 70000 (>16-bit)", ew_pwm(26, 1000, 70000));
    rep_bad("pwm freq 0", ew_pwm(26, 0, 100));
    rep_st("irq_detach(27)", ew_irq_detach(27), "");
    rep_bad("irq_count on detached pin", ew_irq_count(27, &cnt));
}

/* =========================================================================================
 * ADC + DAC
 * ========================================================================================= */
EW_EXPORT("t_adc_dac") void t_adc_dac(void) {
    static const uint8_t adc1[] = { 32, 33, 34, 35, 36, 39 };
    static const uint8_t adc2[] = { 4, 13, 14, 15, 25 };
    unsigned i, k; uint32_t v, mn, mx, sum, t; int st;
    head("ADC / DAC");
    for (i = 0; i < sizeof adc1 + sizeof adc2; i++) {
        int pin = i < sizeof adc1 ? adc1[i] : adc2[i - sizeof adc1];
        mn = 0xFFFFFFFFu; mx = 0; sum = 0; st = EW_OK;
        TIC();
        for (k = 0; k < 16 && st == EW_OK; k++) { st = ew_adc(pin, &v); if (v < mn) mn = v; if (v > mx) mx = v; sum += v; }
        t = TOC();
        if (st == EW_OK) rep(mx <= 4095u ? K_PASS : K_FAIL, i < sizeof adc1 ? "adc1" : "adc2", "pin %2d: min=%u avg=%u max=%u (12-bit), %u us/sample",
                              pin, (unsigned)mn, (unsigned)(sum / 16u), (unsigned)mx, (unsigned)(t / 16u));
        else rep(st == EW_UNSUPPORTED ? K_UNSUP : K_FAIL, i < sizeof adc1 ? "adc1" : "adc2", "pin %2d: %s", pin, ew_status_name(st));
        ew_sleep(1);
    }
    ew_println("note: floating pins read noise; ADC2 pins stop working if WiFi is started");
    rep_bad("adc on non-ADC pin 5", ew_adc(5, &v));
    st = ew_dac(25, 128);
    rep(st == EW_OK ? K_PASS : st == EW_UNSUPPORTED ? K_UNSUP : K_FAIL, "dac(25, 128)", "%s%s", ew_status_name(st),
        st == EW_UNSUPPORTED ? " (the ESP-IDF backend has no DAC implementation)" : "");
}

/* =========================================================================================
 * UART (port 2: TX=17, RX=16)
 * ========================================================================================= */
static const char uart_msg[] = "ECCLES-UART-LOOPBACK-0123456789";
EW_EXPORT("t_uart") void t_uart(void) {
    static const int bauds[] = { 9600, 115200, 921600 };
    uint8_t rx[64]; uint32_t got, a, t; unsigned b; int st, n = (int)sizeof uart_msg - 1;
    head("UART (port 2, TX=GPIO17 RX=GPIO16)");
    for (b = 0; b < 3; b++) {
        st = ew_uart_begin(2, bauds[b], 17, 16);
        if (st != EW_OK) { rep(st == EW_UNSUPPORTED ? K_UNSUP : K_FAIL, "uart_begin", "%d baud -> %s", bauds[b], ew_status_name(st)); continue; }
        memset(rx, 0, sizeof rx); got = 0;
        TIC(); st = ew_uart_write(2, uart_msg, n); t = TOC();
        rep_st("uart_write", st, "");
        a = ew_micros(); ew_uart_read(2, rx, n, 150, &got); a = ew_micros() - a;
        if (got == (uint32_t)n && memcmp(rx, uart_msg, (size_t)n) == 0)
            rep(K_PASS, "uart loopback", "%d baud: %d bytes echoed intact (write %u us, read %u us)", bauds[b], n, (unsigned)t, (unsigned)a);
        else
            rep(K_INFO, "uart loopback", "%d baud: got %u of %d bytes (needs jumper GPIO17<->GPIO16)", bauds[b], (unsigned)got, n);
        rep_st("uart_end", ew_uart_end(2), "");
    }
    st = ew_uart_begin(2, 115200, 17, 16);
    if (st == EW_OK) {
        a = ew_micros(); st = ew_uart_read(2, rx, 8, 100, &got); a = ew_micros() - a;
        rep(st == EW_OK && got == 0 && a >= 95000u && a <= 125000u ? K_PASS : K_FAIL, "uart read timeout", "empty read 100 ms -> got %u after %u us", (unsigned)got, (unsigned)a);
        ew_uart_end(2);
    }
    rep_bad("uart_begin bad port 9", ew_uart_begin(9, 115200, 17, 16));
    rep_bad("uart_begin baud 0", ew_uart_begin(2, 0, 17, 16));
}

/* =========================================================================================
 * I2C (bus 0: SDA=21 SCL=22): bus scan
 * ========================================================================================= */
EW_EXPORT("t_i2c") void t_i2c(void) {
    uint8_t b = 0; int addr, found = 0, st, stuck = 0; uint32_t t, w, wmax = 0;
    head("I2C (bus 0, SDA=GPIO21 SCL=GPIO22, 100 kHz)");
    st = ew_i2c_begin(0, 21, 22, 100000);
    if (rep_st("i2c_begin", st, "") != EW_OK) { rep_bad("i2c_begin bad bus 5", ew_i2c_begin(5, 21, 22, 100000)); return; }
    TIC();
    for (addr = 0x08; addr <= 0x77 && !stuck; addr++) {
        w = ew_micros(); st = ew_i2c_read(0, addr, &b, 1); w = ew_micros() - w;
        if (w > wmax) wmax = w;
        if (w > 200000u) { stuck = 1; break; }
        if (st == EW_OK) { ew_printf("  device found at 0x%02X\n", addr); found++; }
    }
    t = TOC();
    if (stuck) rep(K_INFO, "i2c scan", "aborted: probe took %u us (bus held low / no pull-ups?)", (unsigned)wmax);
    else {
        rep(K_PASS, "i2c scan", "0x08..0x77 scanned in %u us, slowest probe %u us", (unsigned)t, (unsigned)wmax);
        rep(K_INFO, "i2c devices", "%d found%s", found, found ? "" : " (nothing attached - bus itself works: every address NACKed cleanly)");
    }
    rep_bad("i2c_begin bad bus 5", ew_i2c_begin(5, 21, 22, 100000));
    rep(K_INFO, "i2c_end", "no end call exists in the API: the bus stays installed until reboot");
}

/* =========================================================================================
 * SPI (HSPI: SCK=18 MOSI=23 MISO=19, mode 0)
 * ========================================================================================= */
EW_EXPORT("t_spi") void t_spi(void) {
    static const int freqs[] = { 1000000, 8000000 };
    static uint8_t tx[256] __attribute__((aligned(16))), rx[256] __attribute__((aligned(16)));
    unsigned f, i; int st, same; uint32_t t;
    head("SPI (bus 0: SCK=GPIO18 MOSI=GPIO23 MISO=GPIO19)");
    for (i = 0; i < 256; i++) tx[i] = (uint8_t)(i * 5u + 1u);
    for (f = 0; f < 2; f++) {
        st = ew_spi_begin(0, 18, 23, 19, freqs[f], 0);
        if (rep_st("spi_begin", st, freqs[f] == 1000000 ? "1 MHz" : "8 MHz") != EW_OK) continue;
        memset(rx, 0xA5, sizeof rx);
        TIC(); st = ew_spi_transfer(0, tx, rx, 256); t = TOC();
        rep_st("spi_transfer 256 B", st, "");
        rep(K_INFO, "spi speed", "%u us for 256 bytes -> %u kbit/s effective", (unsigned)t, (unsigned)(t ? 256u * 8u * 1000u / t : 0));
        same = (memcmp(tx, rx, 256) == 0);
        if (same) rep(K_PASS, "spi loopback", "256 bytes returned identical at %d Hz", freqs[f]);
        else rep(K_INFO, "spi loopback", "rx differs (needs jumper GPIO23<->GPIO19); rx[0]=0x%02X rx[1]=0x%02X", rx[0], rx[1]);
        rep_st("spi tx-only", ew_spi_transfer(0, tx, 0, 64), "");
        rep_st("spi rx-only", ew_spi_transfer(0, 0, rx, 64), "");
        rep_st("spi_end", ew_spi_end(0), "");
    }
    rep_bad("spi_begin bad bus 7", ew_spi_begin(7, 18, 23, 19, 1000000, 0));
    rep_bad("spi_begin bad mode 5", ew_spi_begin(0, 18, 23, 19, 1000000, 5));
    rep_bad("spi_transfer without begin", ew_spi_transfer(0, tx, rx, 4));
}

