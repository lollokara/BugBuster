<script>
  import Greet from './lib/Greet.svelte'
  import { setItems, setActiveTab, setHidden, setBadge, onTabSelected } from 'tauri-plugin-ios-glass-tabbar-api'

	let response = $state('')

	function log(value) {
		response += `[${new Date().toLocaleTimeString()}] ` + (typeof value === 'string' ? value : JSON.stringify(value)) + '<br>'
	}

	// On iOS (26 SDK) this shows a real Liquid Glass UITabBar; elsewhere it no-ops.
	async function installBar() {
		await setItems([
			{ key: 'chats',    title: 'Messages', sfSymbol: 'message' },
			{ key: 'contacts', title: 'Contacts', sfSymbol: 'person.2' },
			{ key: 'settings', title: 'Settings', sfSymbol: 'gearshape' },
		], 0)
		await onTabSelected((e) => log(`tab selected: ${e.key} (#${e.index})`))
		await setBadge(0, '3')
		log('installed native glass tab bar')
	}
</script>

<main class="container">
  <h1>Welcome to Tauri!</h1>

  <div class="row">
    <a href="https://vite.dev" target="_blank">
      <img src="/vite.svg" class="logo vite" alt="Vite Logo" />
    </a>
    <a href="https://tauri.app" target="_blank">
      <img src="/tauri.svg" class="logo tauri" alt="Tauri Logo" />
    </a>
    <a href="https://svelte.dev" target="_blank">
      <img src="/svelte.svg" class="logo svelte" alt="Svelte Logo" />
    </a>
  </div>

  <p>
    Click on the Tauri, Vite, and Svelte logos to learn more.
  </p>

  <div class="row">
    <Greet />
  </div>

  <div>
    <button onclick={installBar}>Install glass tab bar</button>
    <button onclick={() => setActiveTab(1)}>Select tab 1</button>
    <button onclick={() => setHidden(true)}>Hide</button>
    <button onclick={() => setHidden(false)}>Show</button>
    <div>{@html response}</div>
  </div>

</main>

<style>
  .logo.vite:hover {
    filter: drop-shadow(0 0 2em #747bff);
  }

  .logo.svelte:hover {
    filter: drop-shadow(0 0 2em #ff3e00);
  }
</style>
