use serde::{Deserialize, Serialize};

/// One tab. `sf_symbol` is an SF Symbol name (e.g. "message", "person.2",
/// "gearshape"); `key` is the id echoed back on the `tabSelected` event.
#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct TabItem {
    pub key: String,
    pub title: String,
    pub sf_symbol: String,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SetItemsRequest {
    pub items: Vec<TabItem>,
    #[serde(default)]
    pub selected_index: usize,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SetActiveTabRequest {
    pub index: usize,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SetHiddenRequest {
    pub hidden: bool,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SetBadgeRequest {
    pub index: usize,
    #[serde(default)]
    pub value: Option<String>,
}
