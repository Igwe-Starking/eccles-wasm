//! # eccles-wasm-runtime  (the ECCLES WASM runtime SDK for Rust)
//!
//! Call the host chip's hardware from Rust compiled to `wasm32-unknown-unknown`.
//! Every call returns [`Result`]: `Ok` for SUCCESSFUL, [`Error::Failed`] when the chip tried and
//! failed, [`Error::Unsupported`] when this chip/backend cannot do it at all.
//!
//! ```ignore
//! #![no_std]
//! use eccles_wasm_runtime::{println, Pin, sleep_ms};
//!
//! #[no_mangle] pub extern "C" fn setup() { println!("hello from Rust, millis = {}", eccles_wasm_runtime::millis()); }
//! #[no_mangle] pub extern "C" fn tick()  { let led = Pin(2); let _ = led.toggle(); sleep_ms(500); }
//! ```
//!
//! Build:  `cargo build --release --target wasm32-unknown-unknown`  (see `.cargo/config.toml`).
//!
//! `make sdk-check` compares the import names and arities in this file with the runtime's import table.
#![no_std]
#![allow(clippy::missing_safety_doc)]

use core::fmt;

mod sys {
    // Import names are the host's function names; the module name is ignored by the runtime.
    #[link(wasm_import_module = "env")]
    extern "C" {
        pub fn enable(pin: u32) -> i32;
        pub fn disable(pin: u32) -> i32;
        pub fn toggle(pin: u32) -> i32;
        pub fn mode(pin: u32, mode: u32) -> i32;
        pub fn write(pin: u32, level: u32) -> i32;
        pub fn read(pin: u32, level: *mut u32) -> i32;
        pub fn analog(pin: u32, value: u32) -> i32;
        pub fn pwm(pin: u32, freq_hz: u32, duty16: u32) -> i32;
        pub fn adc(pin: u32, value: *mut u32) -> i32;
        pub fn dac(pin: u32, value: u32) -> i32;
        pub fn sleep(ms: u32) -> i32;
        pub fn sleep_us(us: u32) -> i32;
        pub fn millis(ms: *mut u32) -> i32;
        pub fn micros(us: *mut u32) -> i32;
        #[link_name = "yield"]
        pub fn yield_() -> i32;
        pub fn deep_sleep(ms: u32) -> i32;
        pub fn reset() -> i32;
        pub fn wdt_feed() -> i32;
        pub fn random(value: *mut u32) -> i32;
        pub fn log(buf: *const u8, len: u32) -> i32;
        pub fn irq_attach(pin: u32, edge: u32) -> i32;
        pub fn irq_count(pin: u32, count: *mut u32) -> i32;
        pub fn irq_clear(pin: u32) -> i32;
        pub fn irq_detach(pin: u32) -> i32;
        pub fn uart_begin(port: u32, baud: u32, tx_pin: i32, rx_pin: i32) -> i32;
        pub fn uart_write(port: u32, buf: *const u8, len: u32) -> i32;
        pub fn uart_read(port: u32, buf: *mut u8, len: u32, timeout_ms: u32, got: *mut u32) -> i32;
        pub fn uart_end(port: u32) -> i32;
        pub fn i2c_begin(bus: u32, sda_pin: i32, scl_pin: i32, freq_hz: u32) -> i32;
        pub fn i2c_write(bus: u32, addr7: u32, buf: *const u8, len: u32) -> i32;
        pub fn i2c_read(bus: u32, addr7: u32, buf: *mut u8, len: u32) -> i32;
        pub fn i2c_write_read(bus: u32, addr7: u32, tx: *const u8, txlen: u32, rx: *mut u8, rxlen: u32) -> i32;
        pub fn spi_begin(bus: u32, sck_pin: i32, mosi_pin: i32, miso_pin: i32, freq_hz: u32, mode: u32) -> i32;
        pub fn spi_transfer(bus: u32, tx: *const u8, rx: *mut u8, len: u32) -> i32;
        pub fn spi_end(bus: u32) -> i32;
    }
}

/// Why a hardware call did not succeed.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum Error {
    /// The chip tried and failed (bad pin, bus error, timeout, ...).
    Failed,
    /// This chip / backend cannot do that at all.
    Unsupported,
}
pub type Result<T> = core::result::Result<T, Error>;

#[inline]
fn check(status: i32) -> Result<()> {
    match status {
        0 => Ok(()),
        2 => Err(Error::Unsupported),
        _ => Err(Error::Failed),
    }
}

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum PinMode { Input = 0, Output = 1, InputPullUp = 2, InputPullDown = 3, OpenDrain = 4 }

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum Edge { Rising = 1, Falling = 2, Both = 3 }

/// A GPIO pin, numbered the way the host platform numbers them.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct Pin(pub u32);

impl Pin {
    pub fn mode(self, m: PinMode) -> Result<()> { check(unsafe { sys::mode(self.0, m as u32) }) }
    pub fn output(self) -> Result<()> { self.mode(PinMode::Output) }
    pub fn input(self) -> Result<()> { self.mode(PinMode::Input) }
    pub fn input_pullup(self) -> Result<()> { self.mode(PinMode::InputPullUp) }
    /// Make the pin an output and drive it HIGH.
    pub fn high(self) -> Result<()> { check(unsafe { sys::enable(self.0) }) }
    /// Make the pin an output and drive it LOW.
    pub fn low(self) -> Result<()> { check(unsafe { sys::disable(self.0) }) }
    pub fn toggle(self) -> Result<()> { check(unsafe { sys::toggle(self.0) }) }
    pub fn set(self, level: bool) -> Result<()> { check(unsafe { sys::write(self.0, level as u32) }) }
    pub fn read(self) -> Result<bool> {
        let mut v = 0u32;
        check(unsafe { sys::read(self.0, &mut v) })?;
        Ok(v != 0)
    }
    /// PWM duty, 0..=255.
    pub fn analog(self, duty: u32) -> Result<()> { check(unsafe { sys::analog(self.0, duty) }) }
    /// PWM at an explicit frequency; `duty16` is 0..=65535.
    pub fn pwm(self, freq_hz: u32, duty16: u32) -> Result<()> { check(unsafe { sys::pwm(self.0, freq_hz, duty16) }) }
    /// Raw ADC reading.
    pub fn adc(self) -> Result<u32> {
        let mut v = 0u32;
        check(unsafe { sys::adc(self.0, &mut v) })?;
        Ok(v)
    }
    pub fn dac(self, value: u32) -> Result<()> { check(unsafe { sys::dac(self.0, value) }) }
    /// Count edges in the host (poll with [`Pin::irq_count`]).
    pub fn irq_attach(self, edge: Edge) -> Result<()> { check(unsafe { sys::irq_attach(self.0, edge as u32) }) }
    pub fn irq_count(self) -> Result<u32> {
        let mut v = 0u32;
        check(unsafe { sys::irq_count(self.0, &mut v) })?;
        Ok(v)
    }
    pub fn irq_clear(self) -> Result<()> { check(unsafe { sys::irq_clear(self.0) }) }
    pub fn irq_detach(self) -> Result<()> { check(unsafe { sys::irq_detach(self.0) }) }
}

// ---- time / power ----
pub fn sleep_ms(ms: u32) { unsafe { sys::sleep(ms); } }
pub fn sleep_us(us: u32) { unsafe { sys::sleep_us(us); } }
pub fn millis() -> u32 { let mut v = 0u32; unsafe { sys::millis(&mut v); } v }
pub fn micros() -> u32 { let mut v = 0u32; unsafe { sys::micros(&mut v); } v }
pub fn yield_now() { unsafe { sys::yield_(); } }
pub fn deep_sleep_ms(ms: u32) -> Result<()> { check(unsafe { sys::deep_sleep(ms) }) }
pub fn reset() -> Result<()> { check(unsafe { sys::reset() }) }
pub fn feed_watchdog() -> Result<()> { check(unsafe { sys::wdt_feed() }) }
/// Hardware random number (Unsupported on chips without an RNG).
pub fn random() -> Result<u32> {
    let mut v = 0u32;
    check(unsafe { sys::random(&mut v) })?;
    Ok(v)
}

// ---- console ----
/// Write bytes to the host console (serial / stdout, whatever the host wired up).
pub fn log_bytes(b: &[u8]) { unsafe { sys::log(b.as_ptr(), b.len() as u32); } }
pub fn log(s: &str) { log_bytes(s.as_bytes()); }

/// `core::fmt::Write` sink for the console; used by [`print!`] / [`println!`].
pub struct Console;
impl fmt::Write for Console {
    fn write_str(&mut self, s: &str) -> fmt::Result { log(s); Ok(()) }
}
#[macro_export]
macro_rules! print {
    ($($arg:tt)*) => {{ use ::core::fmt::Write as _; let _ = ::core::write!($crate::Console, $($arg)*); }};
}
#[macro_export]
macro_rules! println {
    () => { $crate::log("\n") };
    ($($arg:tt)*) => {{ use ::core::fmt::Write as _; let _ = ::core::writeln!($crate::Console, $($arg)*); }};
}

// ---- buses ----
#[derive(Copy, Clone, Debug)]
pub struct I2c(pub u32);
impl I2c {
    pub fn begin(self, sda: i32, scl: i32, freq_hz: u32) -> Result<()> { check(unsafe { sys::i2c_begin(self.0, sda, scl, freq_hz) }) }
    pub fn write(self, addr: u8, data: &[u8]) -> Result<()> { check(unsafe { sys::i2c_write(self.0, addr as u32, data.as_ptr(), data.len() as u32) }) }
    pub fn read(self, addr: u8, buf: &mut [u8]) -> Result<()> { check(unsafe { sys::i2c_read(self.0, addr as u32, buf.as_mut_ptr(), buf.len() as u32) }) }
    /// Write then read with a repeated start (register reads).
    pub fn write_read(self, addr: u8, tx: &[u8], rx: &mut [u8]) -> Result<()> {
        check(unsafe { sys::i2c_write_read(self.0, addr as u32, tx.as_ptr(), tx.len() as u32, rx.as_mut_ptr(), rx.len() as u32) })
    }
}

#[derive(Copy, Clone, Debug)]
pub struct Spi(pub u32);
impl Spi {
    pub fn begin(self, sck: i32, mosi: i32, miso: i32, freq_hz: u32, mode: u32) -> Result<()> { check(unsafe { sys::spi_begin(self.0, sck, mosi, miso, freq_hz, mode) }) }
    pub fn write(self, data: &[u8]) -> Result<()> { check(unsafe { sys::spi_transfer(self.0, data.as_ptr(), core::ptr::null_mut(), data.len() as u32) }) }
    pub fn read(self, buf: &mut [u8]) -> Result<()> { check(unsafe { sys::spi_transfer(self.0, core::ptr::null(), buf.as_mut_ptr(), buf.len() as u32) }) }
    /// Full-duplex: `tx` and `rx` must have the same length.
    pub fn transfer(self, tx: &[u8], rx: &mut [u8]) -> Result<()> {
        if tx.len() != rx.len() { return Err(Error::Failed); }
        check(unsafe { sys::spi_transfer(self.0, tx.as_ptr(), rx.as_mut_ptr(), tx.len() as u32) })
    }
    pub fn end(self) -> Result<()> { check(unsafe { sys::spi_end(self.0) }) }
}

#[derive(Copy, Clone, Debug)]
pub struct Uart(pub u32);
impl Uart {
    pub fn begin(self, baud: u32, tx: i32, rx: i32) -> Result<()> { check(unsafe { sys::uart_begin(self.0, baud, tx, rx) }) }
    pub fn write(self, data: &[u8]) -> Result<()> { check(unsafe { sys::uart_write(self.0, data.as_ptr(), data.len() as u32) }) }
    /// Reads up to `buf.len()` bytes, waiting at most `timeout_ms`; returns how many arrived.
    pub fn read(self, buf: &mut [u8], timeout_ms: u32) -> Result<usize> {
        let mut got = 0u32;
        check(unsafe { sys::uart_read(self.0, buf.as_mut_ptr(), buf.len() as u32, timeout_ms, &mut got) })?;
        Ok(got as usize)
    }
    pub fn end(self) -> Result<()> { check(unsafe { sys::uart_end(self.0) }) }
}

#[cfg(feature = "panic-handler")]
#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    print!("panic: ");
    if let Some(l) = info.location() { println!("{}:{}", l.file(), l.line()); } else { println!(); }
    core::arch::wasm32::unreachable()
}

// ---------------------------------------------------------------------------------------------
// embedded-hal 1.0: run any driver crate unmodified inside the module.
#[cfg(feature = "embedded-hal")]
mod hal {
    use super::*;
    use embedded_hal::digital::{ErrorKind as DErr, ErrorType as DType, InputPin, OutputPin};
    use embedded_hal::i2c::{ErrorKind as IErr, ErrorType as IType, I2c as HalI2c, Operation, SevenBitAddress};

    impl embedded_hal::digital::Error for Error { fn kind(&self) -> DErr { DErr::Other } }
    impl embedded_hal::i2c::Error for Error { fn kind(&self) -> IErr { IErr::Other } }

    impl DType for Pin { type Error = Error; }
    impl OutputPin for Pin {
        fn set_low(&mut self) -> Result<()> { (*self).low() }
        fn set_high(&mut self) -> Result<()> { (*self).high() }
    }
    impl InputPin for Pin {
        fn is_high(&mut self) -> Result<bool> { self.read() }
        fn is_low(&mut self) -> Result<bool> { self.read().map(|v| !v) }
    }

    pub struct Delay;
    impl embedded_hal::delay::DelayNs for Delay {
        fn delay_ns(&mut self, ns: u32) { sleep_us((ns + 999) / 1000); }
        fn delay_us(&mut self, us: u32) { sleep_us(us); }
        fn delay_ms(&mut self, ms: u32) { sleep_ms(ms); }
    }

    impl IType for I2c { type Error = Error; }
    impl HalI2c<SevenBitAddress> for I2c {
        fn transaction(&mut self, address: u8, ops: &mut [Operation<'_>]) -> Result<()> {
            for op in ops.iter_mut() {
                match op {
                    Operation::Write(w) => (*self).write(address, w)?,
                    Operation::Read(r) => (*self).read(address, r)?,
                }
            }
            Ok(())
        }
    }
}
#[cfg(feature = "embedded-hal")]
pub use hal::Delay;
