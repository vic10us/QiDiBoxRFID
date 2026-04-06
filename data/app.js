const COLORS = {
  1:'#FAFAFA', 2:'#060606', 3:'#D9E3ED', 4:'#5CF30F', 5:'#63E492',
  6:'#2850FF', 7:'#FE98FE', 8:'#DFD628', 9:'#228332', 10:'#99DEFF',
  11:'#1714B0', 12:'#CEC0FE', 13:'#CADE4B', 14:'#1353AB', 15:'#5EA9FD',
  16:'#A878FF', 17:'#FE717A', 18:'#FF362D', 19:'#E2DFCD', 20:'#898F9B',
  21:'#6E3812', 22:'#CAC59F', 23:'#F28636', 24:'#B87F2B'
};

const MATERIALS = {
  1:'PLA', 2:'PLA Matte', 3:'PLA Metal', 4:'PLA Silk', 5:'PLA-CF',
  6:'PLA-Wood', 7:'PLA Basic', 8:'PLA Matte Basic',
  11:'ABS', 12:'ABS-GF', 13:'ABS-Metal', 14:'ABS-Odorless',
  18:'ASA', 19:'ASA-AERO',
  24:'UltraPA', 25:'PA-CF', 26:'UltraPA-CF25', 27:'PA12-CF',
  30:'PAHT-CF', 31:'PAHT-GF', 32:'Support For PAHT', 33:'Support For PET/PA',
  34:'PC/ABS-FR',
  37:'PET-CF', 38:'PET-GF', 39:'PETG Basic', 40:'PETG Tough',
  41:'PETG Rapido', 42:'PETG-CF', 43:'PETG-GF', 44:'PPS-CF',
  45:'PETG Translucent',
  47:'PVA',
  49:'TPU-Aero', 50:'TPU'
};

const COLOR_NAMES = {
  1:'White', 2:'Black', 3:'Light Grey', 4:'Lime Green', 5:'Mint Green',
  6:'Blue', 7:'Pink', 8:'Yellow', 9:'Dark Green', 10:'Sky Blue',
  11:'Dark Blue', 12:'Lavender', 13:'Yellow-Green', 14:'Navy',
  15:'Cornflower Blue', 16:'Purple', 17:'Salmon', 18:'Red',
  19:'Beige', 20:'Grey', 21:'Brown', 22:'Sand', 23:'Orange', 24:'Gold'
};

let lastTimestamp = 0;
let currentTagData = null;
let modalDismissedTimestamp = 0;
let suppressModal = false;

function toHex(n) {
  return '0x' + n.toString(16).toUpperCase().padStart(2, '0');
}

function updatePreview() {
  const mat = parseInt(document.getElementById('material').value);
  const col = parseInt(document.getElementById('color').value);
  const hex = COLORS[col];

  document.getElementById('byte0').textContent = toHex(mat);
  document.getElementById('byte1').textContent = toHex(col);
  document.getElementById('swatch').style.background = hex;
  document.getElementById('colorHex').textContent = hex;
}

function showWriteStatus(type, icon, msg) {
  const el = document.getElementById('writeStatus');
  el.className = 'status show ' + type;
  document.getElementById('writeStatusIcon').textContent = icon;
  document.getElementById('writeStatusMsg').textContent = msg;
}

async function writeTag() {
  const mat = document.getElementById('material').value;
  const col = document.getElementById('color').value;
  const btn = document.getElementById('writeBtn');

  btn.disabled = true;
  suppressModal = true;
  showWriteStatus('waiting', '\u23F3', 'Waiting for tag \u2014 place it on the reader now...');

  try {
    const res = await fetch('/write?material=' + mat + '&color=' + col);
    const txt = await res.text();

    if (res.ok) {
      showWriteStatus('success', '\u2705', txt);
    } else {
      showWriteStatus('error', '\u274C', txt);
    }
  } catch (e) {
    showWriteStatus('error', '\u274C', 'Network error \u2014 is the ESP32 reachable?');
  }

  // Sync timestamp then release suppress
  try {
    const s = await fetch('/status');
    if (s.ok) { const d = await s.json(); lastTimestamp = d.timestamp || 0; }
  } catch(e2) {}
  suppressModal = false;
  btn.disabled = false;
}

// ── Modal ──

function formatHexDump(hexStr, bytesPerRow, cardTypeId) {
  if (!hexStr) return '(no data)';
  const bytes = hexStr.split(' ');
  let lines = '';
  const isClassic = (cardTypeId === 1);

  for (let i = 0; i < bytes.length; i += bytesPerRow) {
    const chunk = bytes.slice(i, i + bytesPerRow).join(' ');
    let label;
    if (isClassic) {
      const block = Math.floor(i / 16);
      label = 'B' + block.toString().padStart(2, '0');
    } else {
      const page = Math.floor(i / 4);
      label = 'P' + page.toString().padStart(2, '0');
    }
    lines += (lines ? '\n' : '') + label + ':  ' + chunk;
  }
  return lines;
}

function showModal(data) {
  currentTagData = data;
  const modal = document.getElementById('tagModal');
  const warning = document.getElementById('tagWarning');
  const loadBtn = document.getElementById('loadBtn');
  const clearBtn = document.getElementById('clearBtn');

  const cardType = data.cardType || 'Unknown';
  const isClassic = (data.cardTypeId === 1);
  const isUltralight = (data.cardTypeId === 2);

  // UID line with card type and data size
  let uidLine = 'UID: ' + data.uid + '  \u00B7  ' + cardType;
  if (data.dataLen > 0) {
    uidLine += '  \u00B7  ' + data.dataLen + ' bytes';
    if (isClassic && data.blocksRead) uidLine += ' (' + data.blocksRead + ' blocks)';
    if (isUltralight && data.pagesRead) uidLine += ' (' + data.pagesRead + ' pages)';
  }
  document.getElementById('tagUid').textContent = uidLine;

  // Raw data label
  if (isClassic) {
    document.getElementById('rawLabel').textContent = 'Raw Data \u00B7 Sectors 0\u20133';
  } else if (isUltralight) {
    document.getElementById('rawLabel').textContent = 'Raw Data \u00B7 Pages 0\u2013' + (data.pagesRead - 1);
  } else {
    document.getElementById('rawLabel').textContent = 'Raw Data';
  }

  // Format hex dump: 16 bytes/row for Classic, 4 bytes/row for UL/NTAG
  const rowSize = isClassic ? 16 : 4;
  document.getElementById('tagRaw').textContent = formatHexDump(data.raw, rowSize, data.cardTypeId);

  // Default: show clear button for any readable card
  clearBtn.style.display = (data.dataLen > 0 && !data.authFailed) ? 'block' : 'none';

  if (data.authFailed) {
    document.getElementById('modalTitle').textContent = 'Locked Tag';
    document.getElementById('modalSubtitle').textContent = cardType + ' \u00B7 Authentication failed';
    document.getElementById('tagHeader').style.display = 'none';
    document.getElementById('tagBytes').style.display = 'none';
    warning.textContent = 'This tag uses non-default authentication keys. Block data cannot be read.';
    warning.style.display = 'block';
    loadBtn.style.display = 'none';
    clearBtn.style.display = 'none';
    modal.classList.add('show');
    return;
  }

  if (data.readFailed) {
    document.getElementById('modalTitle').textContent = 'Read Error';
    document.getElementById('modalSubtitle').textContent = cardType + ' \u00B7 Read failed';
    document.getElementById('tagHeader').style.display = 'none';
    document.getElementById('tagBytes').style.display = 'none';
    warning.textContent = 'Could not read data from this tag.';
    warning.style.display = 'block';
    loadBtn.style.display = 'none';
    clearBtn.style.display = 'none';
    modal.classList.add('show');
    return;
  }

  if (data.isQidiFormat && isClassic) {
    // ── QIDI format on MIFARE Classic ──
    const matName = MATERIALS[data.material] || ('Unknown Material (' + toHex(data.material) + ')');
    const colName = COLOR_NAMES[data.color] || ('Unknown Color (' + toHex(data.color) + ')');
    const colHex = COLORS[data.color] || '#444444';

    document.getElementById('modalTitle').textContent = 'QIDI Tag Detected';
    document.getElementById('modalSubtitle').textContent = cardType + ' \u00B7 Valid QIDI filament configuration';
    document.getElementById('tagHeader').style.display = 'flex';
    document.getElementById('tagBytes').style.display = 'flex';
    document.getElementById('tagByte0').textContent = toHex(data.material);
    document.getElementById('tagByte1').textContent = toHex(data.color);
    document.getElementById('tagByte2').textContent = toHex(data.manufacturer);
    document.getElementById('tagMaterial').textContent = matName;
    document.getElementById('tagColor').textContent = colName;
    document.getElementById('tagSwatch').style.background = colHex;
    warning.style.display = 'none';

    const canLoad = MATERIALS[data.material] && COLOR_NAMES[data.color];
    loadBtn.style.display = canLoad ? 'block' : 'none';

  } else {
    // ── Non-QIDI or non-Classic: show raw data ──
    let title, subtitle, warnText;

    if (isUltralight) {
      title = 'Ultralight / NTAG Tag';
      subtitle = cardType + ' \u00B7 ' + data.pagesRead + ' pages read';
      warnText = 'This is an Ultralight/NTAG card. QIDI filament data requires MIFARE Classic 1K.';
    } else if (isClassic) {
      title = 'MIFARE Classic Tag';
      subtitle = cardType + ' \u00B7 Non-QIDI data';
      warnText = 'This MIFARE Classic tag does not contain recognized QIDI filament data (Mfr byte \u2260 0x01).';
    } else {
      title = 'NFC Tag Detected';
      subtitle = cardType;
      warnText = 'Unknown card type. Raw data may not be available.';
    }

    document.getElementById('modalTitle').textContent = title;
    document.getElementById('modalSubtitle').textContent = subtitle;
    document.getElementById('tagHeader').style.display = 'none';
    document.getElementById('tagBytes').style.display = 'none';
    warning.textContent = warnText;
    warning.style.display = 'block';
    loadBtn.style.display = 'none';
  }

  modal.classList.add('show');
}

function closeModal() {
  document.getElementById('tagModal').classList.remove('show');
  modalDismissedTimestamp = lastTimestamp;
}

function loadToWriter() {
  if (!currentTagData || !currentTagData.isQidiFormat) return;

  const matSelect = document.getElementById('material');
  const colSelect = document.getElementById('color');

  const matOpt = matSelect.querySelector('option[value="' + currentTagData.material + '"]');
  const colOpt = colSelect.querySelector('option[value="' + currentTagData.color + '"]');

  if (matOpt) matSelect.value = currentTagData.material;
  if (colOpt) colSelect.value = currentTagData.color;

  updatePreview();
  closeModal();

  showWriteStatus('success', '\u2705', 'Loaded from tag: ' +
    (MATERIALS[currentTagData.material] || toHex(currentTagData.material)) + ' / ' +
    (COLOR_NAMES[currentTagData.color] || toHex(currentTagData.color)));
}

async function clearTag() {
  const clearBtn = document.getElementById('clearBtn');
  clearBtn.disabled = true;
  clearBtn.textContent = 'Clearing\u2026';
  suppressModal = true;

  closeModal();

  try {
    const res = await fetch('/clear');
    const txt = await res.text();

    if (res.ok) {
      showWriteStatus('success', '\u2705', txt);
    } else {
      showWriteStatus('error', '\u274C', txt);
    }
  } catch (e) {
    showWriteStatus('error', '\u274C', 'Network error \u2014 is the ESP32 reachable?');
  }

  // Sync timestamp then release suppress
  try {
    const s = await fetch('/status');
    if (s.ok) { const d = await s.json(); lastTimestamp = d.timestamp || 0; }
  } catch(e2) {}
  suppressModal = false;
  clearBtn.disabled = false;
  clearBtn.textContent = 'Clear Tag';
}

// ── Polling ──

async function pollStatus() {
  try {
    const res = await fetch('/status');
    if (!res.ok) return;
    const data = await res.json();

    if (data.timestamp !== lastTimestamp) {
      lastTimestamp = data.timestamp;

      if (data.tagPresent && !suppressModal && data.timestamp !== modalDismissedTimestamp) {
        document.getElementById('scanHint').style.display = 'none';
        showModal(data);
      }
    }
  } catch (e) {
    // Silently ignore poll errors
  }
}

// Init: sync timestamp with server so we don't popup stale data on load
async function init() {
  updatePreview();
  try {
    const res = await fetch('/status');
    if (res.ok) {
      const data = await res.json();
      lastTimestamp = data.timestamp || 0;
    }
  } catch (e) {}
  setInterval(pollStatus, 1500);
}
init();
 