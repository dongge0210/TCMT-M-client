// TCMT Device Attitude 3D — MacBook Air M2 model
// Data source: TCMT-M via IPC MotionClient
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { MotionClient, connectionState } from './ipc-client.js';

/* ── Scene ──────────────────────────────────────────────────── */
const scene = new THREE.Scene();
scene.background = new THREE.Color(0x2a2a3a);

const camera = new THREE.PerspectiveCamera(42, innerWidth / innerHeight, 0.5, 60);
camera.position.set(5, 1.5, 4);

const renderer = new THREE.WebGLRenderer({ antialias: true });
renderer.setSize(innerWidth, innerHeight);
renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
renderer.shadowMap.enabled = true;
renderer.toneMapping = THREE.ACESFilmicToneMapping;
renderer.toneMappingExposure = 1.1;
document.body.appendChild(renderer.domElement);

const controls = new OrbitControls(camera, renderer.domElement);
controls.enableDamping = true;
controls.dampingFactor = 0.15;
controls.target.set(0, 0.3, 0);
controls.zoomSpeed = 4.0;
controls.rotateSpeed = 1.0;
controls.panSpeed = 1.2;
controls.minDistance = 1.5;
controls.maxDistance = 15;
controls.update();

/* ── Lighting ───────────────────────────────────────────────── */
scene.add(new THREE.AmbientLight(0x444466, 3));
const sun = new THREE.DirectionalLight(0xffffff, 4.5);
sun.position.set(5, 8, 3);
sun.castShadow = true;
sun.shadow.mapSize.set(2048, 2048);
sun.shadow.camera.near = 0.5;  sun.shadow.camera.far = 40;
sun.shadow.camera.left = -8;   sun.shadow.camera.right = 8;
sun.shadow.camera.top = 8;     sun.shadow.camera.bottom = -2;
sun.shadow.bias = -0.0002;
scene.add(sun);
scene.add(new THREE.DirectionalLight(0x334466, 1.5)).position.set(-3, 2, -2);
scene.add(new THREE.DirectionalLight(0x8899cc, 0.6)).position.set(0, 1, -5);

/* ── Materials ──────────────────────────────────────────────── */
const ALU = new THREE.MeshStandardMaterial({ color: 0xc8c8d0, roughness: 0.18, metalness: 0.95 });
const ALU_D = new THREE.MeshStandardMaterial({ color: 0x9999a2, roughness: 0.22, metalness: 0.92 });
const ALU_L = new THREE.MeshStandardMaterial({ color: 0xd0d0d8, roughness: 0.15, metalness: 0.97 });
const BLACK = new THREE.MeshStandardMaterial({ color: 0x1a1a1a, roughness: 0.4, metalness: 0.02 });
const RUBBER = new THREE.MeshStandardMaterial({ color: 0x1a1a1a, roughness: 0.95, metalness: 0 });
const HINGE = new THREE.MeshStandardMaterial({ color: 0x888890, roughness: 0.12, metalness: 0.96 });

/* ── Geometry helpers ───────────────────────────────────────── */
function roundedRectShape(w, h, r) {
  const s = new THREE.Shape();
  s.moveTo(-w / 2 + r, -h / 2);
  s.lineTo(w / 2 - r, -h / 2);   s.quadraticCurveTo(w / 2, -h / 2, w / 2, -h / 2 + r);
  s.lineTo(w / 2, h / 2 - r);    s.quadraticCurveTo(w / 2, h / 2, w / 2 - r, h / 2);
  s.lineTo(-w / 2 + r, h / 2);   s.quadraticCurveTo(-w / 2, h / 2, -w / 2, h / 2 - r);
  s.lineTo(-w / 2, -h / 2 + r);  s.quadraticCurveTo(-w / 2, -h / 2, -w / 2 + r, -h / 2);
  return s;
}

function roundedExtruded(w, h, d, r, mat, bevelT = 0.02) {
  const shape = roundedRectShape(w, h, r);
  const geo = new THREE.ExtrudeGeometry(shape, {
    depth: d, bevelEnabled: true,
    bevelThickness: bevelT, bevelSize: bevelT, bevelSegments: 3,
  });
  const mesh = new THREE.Mesh(geo, mat);
  mesh.castShadow = true;
  mesh.receiveShadow = true;
  return mesh;
}

/* ── MacBook Air M2 — 30.41×21.5×1.13cm ─────── */
const BW = 3.041, BD = 2.15, BH = 0.113, R = 0.045;

const macbook = new THREE.Group();
scene.add(macbook);

// Base: ExtrudeGeometry Z → rotate to flat (XZ plane), thickness = Y
const baseBody = roundedExtruded(BW, BD, BH, R, ALU);
baseBody.rotation.x = -Math.PI / 2;
macbook.add(baseBody);

// Keyboard well (flat on top of base)
const wellShape = roundedRectShape(BW - 0.55, BD - 1.0, 0.06);
const wellGeo = new THREE.ExtrudeGeometry(wellShape, { depth: 0.025, bevelEnabled: true, bevelThickness: 0.008, bevelSize: 0.008, bevelSegments: 2 });
const well = new THREE.Mesh(wellGeo, ALU_D);
well.position.y = BH + 0.001;
well.rotation.x = -Math.PI / 2;
macbook.add(well);

// Keys
const keyGeo = new THREE.BoxGeometry(0.14, 0.003, 0.135);
for (let r = 0; r < 6; r++) {
  const nc = [14, 14, 13, 13, 12, 11][r];
  for (let k = 0; k < nc; k++) {
    const key = new THREE.Mesh(keyGeo.clone(), BLACK);
    key.position.set((k - (nc - 1) / 2) * 0.163, BH + 0.004, -0.62 + r * 0.185);
    if (r === 5 && k === 6) key.scale.z = 1.4;
    macbook.add(key);
  }
}

// Trackpad
const tpShape = roundedRectShape(1.18, 0.76, 0.08);
const tpGeo = new THREE.ExtrudeGeometry(tpShape, { depth: 0.004, bevelEnabled: true, bevelThickness: 0.004, bevelSize: 0.004, bevelSegments: 2 });
const tpMat = new THREE.MeshStandardMaterial({ color: 0xaaaaaf, roughness: 0.12, metalness: 0.65 });
const trackpad = new THREE.Mesh(tpGeo, tpMat);
trackpad.position.set(0, BH + 0.001, 0.7);
trackpad.rotation.x = -Math.PI / 2;
macbook.add(trackpad);

// Feet
for (const [fx, fz] of [[-1.25, -0.8], [1.25, -0.8], [-1.25, 0.8], [1.25, 0.8]]) {
  const foot = new THREE.Mesh(new THREE.CylinderGeometry(0.05, 0.05, 0.012, 12), RUBBER);
  foot.position.set(fx, 0.002, fz);
  macbook.add(foot);
}

// ── Hinge (disabled — debugging base tilt) ──
const hingePivot = new THREE.Group();
hingePivot.position.set(0, BH, BD / 2 - 0.03);
hingePivot.visible = false;
macbook.add(hingePivot);

const hinge = new THREE.Mesh(new THREE.CylinderGeometry(0.032, 0.032, BW - 0.5, 20), HINGE);
hinge.rotation.z = Math.PI / 2;
hingePivot.add(hinge);

// ── Lid body (flat, pivots around X from back edge) ──
const LW = BW, LD = BD - 0.01, LT = 0.044;
const lidShape = roundedRectShape(LW, LD, R + 0.01);
const lidGeo = new THREE.ExtrudeGeometry(lidShape, { depth: LT, bevelEnabled: true, bevelThickness: 0.012, bevelSize: 0.012, bevelSegments: 3 });
const lidBody = new THREE.Mesh(lidGeo, ALU_L);
lidBody.rotation.x = -Math.PI / 2;
lidBody.position.set(0, 0, -LD / 2);
lidBody.castShadow = true; lidBody.receiveShadow = true;
hingePivot.add(lidBody);

// Lid angle set by sensor data on first update() call


/* ── Axis gizmo (3D RGB arrows on model corner) ────────────── */
const axisGizmo = new THREE.Group();
axisGizmo.position.set(BW/2 - 0.2, BH + 0.6, BD/2 - 0.2);
macbook.add(axisGizmo);
function addAxis(color, rot, len) {
  const g = new THREE.Group();
  const cone = new THREE.Mesh(new THREE.ConeGeometry(0.04, 0.12, 6, 1), new THREE.MeshStandardMaterial({color, roughness:0.1, emissive:color, emissiveIntensity:0.5}));
  cone.position.y = len;
  const rod = new THREE.Mesh(new THREE.CylinderGeometry(0.015, 0.015, len, 6), new THREE.MeshStandardMaterial({color, roughness:0.1, emissive:color, emissiveIntensity:0.3}));
  rod.position.y = len/2;
  g.add(cone, rod);
  if (rot) g.rotation.set(rot[0], rot[1], rot[2]);
  return g;
}
axisGizmo.add(addAxis(0xff3333, null, 0.35));        // +Y = Red (Up)
axisGizmo.add(addAxis(0x33ff33, [Math.PI/2,0,0], 0.35)); // +X = Green (Right)
axisGizmo.add(addAxis(0x3388ff, [0,0,-Math.PI/2], 0.35)); // +Z = Blue (Forward)

/* ── Floor ──────────────────────────────────────────────────── */
const floor = new THREE.Mesh(
  new THREE.PlaneGeometry(20, 20),
  new THREE.MeshStandardMaterial({ color: 0x222233, roughness: 0.7, metalness: 0.1 }),
);
floor.rotation.x = -Math.PI / 2;
floor.position.y = -1.5;
floor.receiveShadow = true;
scene.add(floor);

/* ── HUD & Panel builders ───────────────────────────────────── */
function buildPanel() {
  const p = document.getElementById('panel');
  p.replaceChildren();
  const add = (l, id, cls = 'v') => {
    const r = document.createElement('div'); r.className = 'row';
    const s1 = document.createElement('span'); s1.className = 'l'; s1.textContent = l;
    const s2 = document.createElement('span'); s2.className = cls; s2.id = id; s2.textContent = '—';
    r.append(s1, s2);
    p.appendChild(r);
  };
  add('Accel X', 'ax'); add('Accel Y', 'ay'); add('Accel Z', 'az');
  const s1 = document.createElement('div'); s1.className = 'sec'; s1.textContent = 'm/s²'; p.appendChild(s1);
  add('Gyro X', 'gx'); add('Gyro Y', 'gy'); add('Gyro Z', 'gz');
  const s2 = document.createElement('div'); s2.className = 'sec'; s2.textContent = 'deg/s'; p.appendChild(s2);
  add('Lid Angle', 'lid');
  add('Heartbeat', 'hb');
  add('IMU Temp', 'imut');
}
buildPanel();

/* ── Update from sensor data ────────────────────────────────── */
export function update(data) {
  const { ax = 0, ay = 0, az = -1, gx = 0, gy = 0, gz = 0, lidAngle, hb, imut } = data;

  // gravity → tilt angles
  // Device frame: +X=right, +Y=forward, +Z=up. 3D world: X=right, Y=up, Z=toward.
  const pitch = Math.atan2(-ax, -az) * (180 / Math.PI);  // tilt forward/back
  const roll  = Math.atan2(ay, -az) * (180 / Math.PI);   // tilt left/right

  // Model lies in XZ plane (long edge=X, short edge=Z, thickness=Y)
  // Pitch = rotate around device-X (world-X), Roll = rotate around device-Y (world-Z)
  macbook.rotation.order = 'XZY';
  macbook.rotation.x = roll * (Math.PI / 180);
  macbook.rotation.z = -pitch * (Math.PI / 180);
  // Lid disabled — testing base tilt only

  // HUD
  const el = id => document.getElementById(id);
  if (el('pitchVal')) el('pitchVal').textContent = pitch.toFixed(1) + '°';
  if (el('rollVal')) el('rollVal').textContent = roll.toFixed(1) + '°';
  if (el('ax')) el('ax').textContent = ax.toFixed(3);
  if (el('ay')) el('ay').textContent = ay.toFixed(3);
  if (el('az')) el('az').textContent = az.toFixed(3);
  if (el('gx')) el('gx').textContent = gx.toFixed(1);
  if (el('gy')) el('gy').textContent = gy.toFixed(1);
  if (el('gz')) el('gz').textContent = gz.toFixed(1);
  if (el('lid')) el('lid').textContent = lidAngle != null ? lidAngle.toFixed(0) + '°' : '—';
  if (el('hb')) el('hb').textContent = hb ?? '—';
  if (el('imut')) el('imut').textContent = imut != null ? imut.toFixed(1) + '°C' : '—';
  if (document.getElementById('status'))
    document.getElementById('status').textContent = 'TCMT · Live';
}

/* ── IPC: MotionClient ─────────────────────────────────────── */
const motion = new MotionClient();
motion.onData(data => update(data));
motion.start(250);

// Connection status heartbeat
setInterval(async () => {
  const st = await connectionState();
  const el = document.getElementById('status');
  if (el) el.textContent = 'TCMT · ' + st;
}, 2000);

/* ── Render ─────────────────────────────────────────────────── */
const clock = new THREE.Clock();
(function loop() {
  requestAnimationFrame(loop);
  controls.update();
  renderer.render(scene, camera);
})();

// Default render — model visible even without sensor data
update({ ax: 0, ay: 0, az: -1 });
console.log('TCMT Attitude 3D ready — MacBook Air M2');

addEventListener('resize', () => {
  camera.aspect = innerWidth / innerHeight;
  camera.updateProjectionMatrix();
  renderer.setSize(innerWidth, innerHeight);
});
