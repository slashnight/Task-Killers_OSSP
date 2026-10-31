const { spawn, exec } = require('child_process');
const http = require('http');

function checkServer() {
  return new Promise((resolve) => {
    const req = http.get('http://127.0.0.1:8080/api/status', (res) => {
      resolve(res.statusCode === 200);
    });
    req.on('error', () => resolve(false));
    req.setTimeout(1000, () => {
      req.destroy();
      resolve(false);
    });
  });
}

function openBrowser(url) {
  if (process.platform === 'win32') {
    // On Windows, the start command treats the first quoted argument as the window title.
    // Using start "" "url" correctly launches the default web browser.
    exec(`start "" "${url}"`, (err) => {
      if (err) {
        exec(`explorer "${url}"`, () => {});
      }
    });
  } else if (process.platform === 'darwin') {
    exec(`open "${url}"`);
  } else {
    exec(`xdg-open "${url}"`);
  }
}

let activeChild = null;
let isStarting = false;

function spawnSupervisor() {
  if (isStarting) return;
  isStarting = true;
  console.log('Starting C supervisor backend inside WSL Ubuntu...');
  
  activeChild = spawn('wsl.exe', [
    '-d', 'Ubuntu', '--', 'bash', '-c',
    'chmod +x run.sh && ./run.sh'
  ], {
    stdio: 'inherit'
  });

  activeChild.on('error', (err) => {
    console.error('Failed to launch WSL:', err.message);
    isStarting = false;
  });

  activeChild.on('exit', (code) => {
    activeChild = null;
    isStarting = false;
    if (code !== 0) {
      console.log(`Supervisor process exited with code ${code}`);
    }
  });

  // Poll until online
  (async () => {
    for (let i = 0; i < 20; i++) {
      await new Promise((r) => setTimeout(r, 1000));
      if (await checkServer()) {
        console.log('✓ C Supervisor is ONLINE at http://localhost:8080/!');
        openBrowser('http://localhost:8080/');
        break;
      }
    }
    isStarting = false;
  })();
}

async function main() {
  console.log('===============================================================');
  console.log('  Linux Service & Process Supervisor — Local Dev Launcher');
  console.log('===============================================================');

  const isRunning = await checkServer();
  if (isRunning) {
    console.log('✓ C Supervisor backend is ALREADY running and online at http://localhost:8080/');
    console.log('Opening dashboard in your default web browser...');
    openBrowser('http://localhost:8080/');
  } else {
    spawnSupervisor();
  }

  console.log('\nWeb Dashboard: http://localhost:8080/');
  console.log('Press Ctrl+C to stop watching.');

  // Continually monitor health and auto-restart if it drops offline
  setInterval(async () => {
    const healthy = await checkServer();
    if (!healthy && !isStarting && !activeChild) {
      console.log('⚠️ Supervisor backend went offline. Automatically restarting...');
      spawnSupervisor();
    }
  }, 4000);
}

main();
