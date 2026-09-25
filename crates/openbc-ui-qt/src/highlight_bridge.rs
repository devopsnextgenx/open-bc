use openbc_core::text::highlight_to_html as render_highlight_to_html;

#[cxx_qt::bridge]
mod highlight_bridge {
    extern "RustQt" {
        #[qobject]
        #[qml_element]
        type HighlightBridge = super::HighlightBridgeRust;

        #[qinvokable]
        fn highlight_to_html(
            self: &HighlightBridge,
            code: &str,
            language: &str,
            is_dark_mode: bool,
        ) -> String;
    }
}

#[derive(Default)]
pub struct HighlightBridgeRust;

impl highlight_bridge::HighlightBridge {
    fn highlight_to_html(&self, code: &str, language: &str, is_dark_mode: bool) -> String {
        render_highlight_to_html(code, language, is_dark_mode)
    }
}
