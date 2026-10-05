var busy = false;

function msg(t) {
  document.getElementById("msg").textContent = t;
}

function scan() {
  if (busy) return;
  msg("扫描中...");
  fetch("/scan").then(function (r) { return r.json(); }).then(function (d) {
    var box = document.getElementById("aplist");
    box.innerHTML = "";
    d.aps.forEach(function (a) {
      var div = document.createElement("div");
      div.className = "ap";
      div.textContent = a.ssid + "  " + a.rssi + "dBm" + (a.auth ? "  🔒" : "");
      div.onclick = function () { document.getElementById("ssid").value = a.ssid; };
      box.appendChild(div);
    });
    if (!d.aps.length) msg("未发现网络");
    else msg("");
  }).catch(function () { msg("扫描失败，请重试"); });
}

function save() {
  var s = document.getElementById("ssid").value.trim();
  if (!s) { msg("请输入网络名称"); return; }
  msg("提交中...");
  busy = true;
  fetch("/save", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ ssid: s, pass: document.getElementById("pass").value })
  }).then(function () { poll(); })
    .catch(function () { msg("提交失败"); busy = false; });
}

function poll() {
  fetch("/status").then(function (r) { return r.json(); }).then(function (d) {
    if (d.state == "success") {
      msg("配置成功！设备已联网");
    } else if (d.state == "failed") {
      msg("连接失败，请检查密码后重试");
      busy = false;
    } else {
      msg("连接中...");
      setTimeout(poll, 2000);
    }
  }).catch(function () { setTimeout(poll, 2000); });
}

window.onload = function () { scan(); };
