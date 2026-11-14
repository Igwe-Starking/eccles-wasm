//! cd sdk/rust/examples/blink && cargo build --release --target wasm32-unknown-unknown
//! -> target/wasm32-unknown-unknown/release/blink.wasm  (copy to the chip's filesystem)
#![no_std]
use eccles_wasm_runtime::{millis, println, sleep_ms, Edge, Error, Pin};

const LED: Pin = Pin(2);
const BUTTON: Pin = Pin(0);

#[no_mangle]
pub extern "C" fn setup() {
    println!("blink (Rust) up at {} ms", millis());
    let _ = LED.output();
    // Unsupported is a normal answer on some chips - handle it instead of crashing.
    match BUTTON.input_pullup().and_then(|_| BUTTON.irq_attach(Edge::Falling)) {
        Ok(()) => println!("button interrupts on"),
        Err(Error::Unsupported) => println!("this chip has no pin-interrupt support"),
        Err(Error::Failed) => println!("button setup failed"),
    }
}

#[no_mangle]
pub extern "C" fn tick() {
    let _ = LED.toggle();
    if let Ok(n) = BUTTON.irq_count() {
        if n > 0 { println!("button pressed {} time(s)", n); let _ = BUTTON.irq_clear(); }
    }
    sleep_ms(250);
}
