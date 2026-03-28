use leptos::prelude::*;

#[component]
pub fn UartTab() -> impl IntoView {
    // UART bridge config will be implemented when the backend commands are ready
    view! {
        <div class="tab-content">
            <div class="card" style="max-width: 600px; margin: 0 auto; text-align: center; padding: 40px;">
                <h3>"UART Bridge Configuration"</h3>
                <p class="hint">"Configure UART bridge settings from the web interface for now."</p>
                <p class="hint">"The bridge transparently connects USB CDC #1 to the ESP32 UART peripheral."</p>
            </div>
        </div>
    }
}
