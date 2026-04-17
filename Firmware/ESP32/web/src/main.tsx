import { render } from "preact";
import "./styles/tokens.css";
import "./styles/components.css";
import { App } from "./App";

const root = document.getElementById("app");
if (!root) throw new Error("#app root element missing");
render(<App />, root);
