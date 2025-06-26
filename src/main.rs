use embassy_executor::{Executor, Spawner};
use embassy_time::{Duration, Timer};
use esp_idf_svc::hal::{gpio::*, peripheral::*, prelude::Peripherals};
use static_cell::StaticCell;

static EXECUTOR: StaticCell<Executor> = StaticCell::new();

fn main() {
    // It is necessary to call this function once. Otherwise some patches to the runtime
    // implemented by esp-idf-sys might not link properly. See https://github.com/esp-rs/esp-idf-template/issues/71
    esp_idf_svc::sys::link_patches();

    // Bind the log crate to the ESP Logging facilities
    esp_idf_svc::log::EspLogger::initialize_default();

    log::info!("hali");

    let executor = Executor::new();
    let executor: &'static mut Executor = EXECUTOR.init(executor);

    executor.run(|spawner| spawner.spawn(main_task(spawner)).unwrap());
}

#[embassy_executor::task]
async fn main_task(spawner: Spawner) {
    let p = Peripherals::take().unwrap();
    let pin = p.pins.gpio12;
    // Spawned tasks run in the background, concurrently.
    log::info!("spawn");
    spawner.spawn(blink(pin.into())).unwrap();
    loop {
        Timer::after_millis(150).await;
        log::info!("11");
    }
}

#[embassy_executor::task]
async fn blink(pin: AnyOutputPin) {
    let mut led = PinDriver::output(pin).unwrap();
    loop {
        // Timekeeping is globally available, no need to mess with hardware timers.
        led.set_high().unwrap();
        Timer::after_millis(150).await;
        log::info!("high");
        led.set_low().unwrap();
        Timer::after_millis(150).await;
        log::info!("low");
    }
}
