//! Plain (unhighlighted) HTML fallback.

/// Source wrapped in an escaped `<pre>`.
pub(super) fn plain_html(source: &str) -> String {
    format!("<pre>{}</pre>", escape_html(source))
}

fn escape_html(source: &str) -> String {
    source
        .replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
        .replace('\'', "&#39;")
}
