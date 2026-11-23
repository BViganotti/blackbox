// Blackbox local workbench. No frameworks, no external resources: every byte
// of this file is served from the binary on 127.0.0.1.
(() => {
  "use strict";

  const $ = (selector) => document.querySelector(selector);

  const state = {
    token: "",
    view: "live",
    minutes: 15,
    query: "",
    queryError: "",
    queryHistory: [],
    snapshot: null,
    records: [],
    nodes: [],
    nodeById: new Map(),
    seqToNode: new Map(),
    dumps: [],
    dumpMeta: null,
    compareLeft: "",
    compareRight: "",
    compareResult: null,
    compareError: "",
    graphFocus: "failure",
    selectedSeq: null,
    activeTab: "failure",
    requestSerial: 0,
    stream: null,
    poll: null,
  };

  const graph = {
    canvas: null,
    ctx: null,
    width: 0,
    height: 0,
    scale: 1,
    offsetX: 0,
    offsetY: 0,
    autoFit: true,
    dragging: false,
    dragMoved: false,
    dragStartX: 0,
    dragStartY: 0,
    startOffsetX: 0,
    startOffsetY: 0,
    rects: [],
    rootCauseId: null,
  };

  const timeline = {
    canvas: null,
    ctx: null,
    width: 0,
    height: 0,
    hits: [],
  };

  const NODE_WIDTH = 168;
  const NODE_HEIGHT = 34;

  // ---------------------------------------------------------------- utilities

  function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined && text !== null) node.textContent = String(text);
    return node;
  }

  function clear(node) {
    while (node.firstChild) node.removeChild(node.firstChild);
  }

  function formatDuration(ns) {
    const value = Number(ns) || 0;
    if (value < 1000) return `${value}ns`;
    if (value < 1e6) return `${(value / 1e3).toFixed(value < 1e4 ? 1 : 0)}µs`;
    if (value < 1e9) return `${(value / 1e6).toFixed(value < 1e7 ? 1 : 0)}ms`;
    return `${(value / 1e9).toFixed(2)}s`;
  }

  function formatRelative(ns) {
    const value = Number(ns) || 0;
    const sign = value < 0 ? "-" : "";
    const abs = Math.abs(value);
    if (abs < 1e6) return `${sign}${(abs / 1e3).toFixed(1)}µs`;
    if (abs < 1e9) return `${sign}${(abs / 1e6).toFixed(1)}ms`;
    return `${sign}${(abs / 1e9).toFixed(2)}s`;
  }

  function formatBytes(bytes) {
    const value = Number(bytes) || 0;
    if (value < 1024) return `${value} B`;
    if (value < 1024 * 1024) return `${(value / 1024).toFixed(1)} KiB`;
    return `${(value / (1024 * 1024)).toFixed(1)} MiB`;
  }

  function formatClock(iso) {
    if (!iso) return "—";
    const date = new Date(iso);
    if (Number.isNaN(date.getTime())) return iso;
    try {
      return date.toLocaleTimeString([], {
        hour12: false,
        hour: "2-digit",
        minute: "2-digit",
        second: "2-digit",
        fractionalSecondDigits: 3,
      });
    } catch {
      return iso.slice(11, 23);
    }
  }

  function formatValue(value) {
    if (value === null || value === undefined) return "null";
    if (typeof value === "string") return value;
    if (typeof value === "boolean") return value ? "true" : "false";
    return String(value);
  }

  function truncate(text, limit) {
    const value = String(text ?? "");
    return value.length > limit ? `${value.slice(0, limit - 1)}…` : value;
  }

  function showToast(message, kind = "") {
    const toast = $("#toast");
    if (!toast) return;
    toast.textContent = message;
    toast.className = `toast ${kind}`.trim();
    clearTimeout(showToast.timer);
    showToast.timer = setTimeout(() => toast.classList.add("hidden"), 5200);
  }

  function setLive(label, mode) {
    const dot = $("#live-dot");
    const text = $("#live-label");
    if (text) text.textContent = label;
    if (dot) dot.className = `dot ${mode}`.trim();
  }

  function readCookie(name) {
    const parts = document.cookie.split(";");
    for (const part of parts) {
      const trimmed = part.trim();
      const equals = trimmed.indexOf("=");
      if (equals > 0 && trimmed.slice(0, equals) === name) {
        return decodeURIComponent(trimmed.slice(equals + 1));
      }
    }
    return "";
  }

  // ---------------------------------------------------------------- HTTP

  const kQueryHistoryKey = "blackbox.queryHistory";

  function loadQueryHistory() {
    try {
      const raw = localStorage.getItem(kQueryHistoryKey);
      if (!raw) return [];
      const parsed = JSON.parse(raw);
      if (!Array.isArray(parsed)) return [];
      return parsed
        .filter((item) => typeof item === "string" && item.trim().length > 0)
        .slice(0, 12);
    } catch {
      return [];
    }
  }

  function saveQueryHistory() {
    try {
      localStorage.setItem(kQueryHistoryKey, JSON.stringify(state.queryHistory));
    } catch {
      // Private mode or a full quota: history is optional.
    }
  }

  function rememberQuery(query) {
    const value = String(query || "").trim();
    if (!value) return;
    state.queryHistory = [
      value,
      ...state.queryHistory.filter((item) => item !== value),
    ].slice(0, 12);
    saveQueryHistory();
  }

  async function requestJson(url, method = "GET") {
    const target = new URL(url, location.origin);
    target.searchParams.set("token", state.token);
    const headers = {};
    if (method !== "GET") headers["X-Blackbox-Token"] = state.token;
    const response = await fetch(target, {
      method,
      headers,
      cache: "no-store",
    });
    let data = null;
    try {
      data = await response.json();
    } catch {
      throw new Error(`HTTP ${response.status}`);
    }
    if (!response.ok || data.ok === false) {
      throw new Error(data && data.error ? data.error : `HTTP ${response.status}`);
    }
    return data;
  }

  async function loadSnapshot(options = {}) {
    if (state.view !== "live") return false;
    const serial = ++state.requestSerial;
    const url = new URL("/api/snapshot", location.origin);
    url.searchParams.set("minutes", String(state.minutes));
    url.searchParams.set("limit", "1200");
    if (state.query) url.searchParams.set("q", state.query);
    try {
      const data = await requestJson(url);
      if (serial !== state.requestSerial || state.view !== "live") return false;
      state.queryError = "";
      markQueryInvalid(false);
      applySnapshot(data);
      return true;
    } catch (error) {
      if (serial !== state.requestSerial) return false;
      if (!options.silent) {
        state.queryError = error.message;
        markQueryInvalid(true);
        showTab("query");
        showToast(error.message, "error");
        renderQueryPanel();
      }
      return false;
    }
  }

  async function loadDumps(options = {}) {
    try {
      const data = await requestJson("/api/dumps");
      state.dumps = Array.isArray(data.dumps) ? data.dumps : [];
      renderDumps();
    } catch (error) {
      if (!options.silent) showToast(`Cannot list dumps: ${error.message}`, "error");
    }
  }

  async function dumpNow() {
    try {
      const data = await requestJson("/api/dump", "POST");
      showToast(`Dump written: ${data.name || "ok"}`, "ok");
      await loadDumps({ silent: true });
      showTab("dumps");
    } catch (error) {
      showToast(`Dump failed: ${error.message}`, "error");
    }
  }

  async function triggerDemo() {
    try {
      await requestJson("/api/demo/trigger", "POST");
      showToast("Demo scenario requested", "ok");
    } catch (error) {
      showToast(`Demo unavailable: ${error.message}`, "error");
    }
  }

  async function openDump(name) {
    try {
      const url = new URL("/api/dump/open", location.origin);
      url.searchParams.set("name", name);
      const data = await requestJson(url);
      state.view = "dump";
      state.dumpMeta = data.dump || { name };
      state.queryError = "";
      state.graphFocus = "failure";
      stopStream();
      stopPolling();
      setLive("dump", "");
      applySnapshot(data);
      const banner = $("#dump-banner");
      if (banner) {
        clear(banner);
        const text = el(
          "span",
          "",
          `Viewing dump ${data.dump.name} · ${data.dump.record_count} records` +
            (data.dump.complete ? "" : ` · ${data.dump.error || "incomplete"}`),
        );
        const live = el("button", "ghost small", "Back to live");
        live.addEventListener("click", goLive);
        banner.appendChild(text);
        banner.appendChild(live);
        banner.classList.remove("hidden");
      }
      const liveButton = $("#live-button");
      if (liveButton) liveButton.classList.remove("hidden");
      showTab("failure");
      showToast(`Opened ${name}`, "ok");
    } catch (error) {
      showToast(`Cannot open dump: ${error.message}`, "error");
    }
  }

  function downloadDump(name) {
    const url = new URL("/api/dump", location.origin);
    url.searchParams.set("name", name);
    url.searchParams.set("token", state.token);
    const anchor = document.createElement("a");
    anchor.href = url.toString();
    anchor.download = name;
    document.body.appendChild(anchor);
    anchor.click();
    anchor.remove();
  }

  function goLive() {
    state.view = "live";
    state.dumpMeta = null;
    state.selectedSeq = null;
    state.graphFocus = "failure";
    $("#dump-banner")?.classList.add("hidden");
    $("#live-button")?.classList.add("hidden");
    graph.autoFit = true;
    loadSnapshot();
    loadDumps({ silent: true });
    startStream();
  }

  // ---------------------------------------------------------------- snapshot

  function applySnapshot(data, options = {}) {
    state.snapshot = data;
    state.records = Array.isArray(data.records) ? data.records : [];
    state.nodes = data.graph && Array.isArray(data.graph.nodes) ? data.graph.nodes : [];
    state.nodeById = new Map(state.nodes.map((node) => [String(node.id), node]));
    state.seqToNode = new Map(state.nodes.map((node) => [Number(node.seq), node.id]));
    const failure = data.failure || {};
    graph.rootCauseId = failure.found ? failure.root_cause : null;
    if (state.selectedSeq !== null && !state.seqToNode.has(Number(state.selectedSeq))) {
      state.selectedSeq = null;
    }
    if (!options.quiet) graph.autoFit = true;
    renderAll();
    if (state.selectedSeq !== null) {
      const row = document.querySelector(`[data-seq="${state.selectedSeq}"]`);
      if (row && row.scrollIntoView) row.scrollIntoView({ block: "nearest" });
    }
  }

  function graphScope() {
    const allEdges = state.snapshot?.graph?.edges || [];
    const failure = state.snapshot?.failure;
    if (
      state.graphFocus !== "failure" ||
      !failure ||
      !failure.found ||
      !Array.isArray(failure.chain) ||
      failure.chain.length < 2
    ) {
      return { nodes: state.nodes, edges: allEdges, focused: false };
    }
    const nodes = failure.chain
      .map((id) => state.nodeById.get(String(id)))
      .filter(Boolean);
    if (nodes.length < 2) {
      return { nodes: state.nodes, edges: allEdges, focused: false };
    }
    const visible = new Set(nodes.map((node) => Number(node.id)));
    const edges = allEdges.filter(
      (edge) => visible.has(Number(edge.from)) && visible.has(Number(edge.to)),
    );
    return { nodes, edges, focused: true };
  }

  function updateGraphFocusButton() {
    const button = $("#graph-focus");
    if (!button) return;
    const scope = graphScope();
    button.textContent = scope.focused ? "Scope: failure" : "Scope: all";
    button.title = scope.focused
      ? "Showing the latest failure chain. Click for every retained record."
      : "Showing every retained record. Click to focus the latest failure.";
  }

  function currentNode() {
    if (state.selectedSeq === null) return null;
    const id = state.seqToNode.get(Number(state.selectedSeq));
    if (id === undefined) return null;
    return state.nodeById.get(String(id)) || null;
  }

  function currentRecord() {
    const node = currentNode();
    if (!node) return null;
    return state.records[node.id] || null;
  }

  function selectSeq(seq, options = {}) {
    state.selectedSeq = seq === null ? null : Number(seq);
    const failure = state.snapshot?.failure;
    if (
      state.selectedSeq !== null &&
      state.graphFocus === "failure" &&
      failure &&
      failure.found &&
      Array.isArray(failure.chain)
    ) {
      const chainSeqs = new Set(
        failure.chain
          .map((id) => state.nodeById.get(String(id)))
          .filter(Boolean)
          .map((node) => Number(node.seq)),
      );
      if (!chainSeqs.has(Number(state.selectedSeq))) {
        // The user picked an event outside the focused chain; widen the graph.
        state.graphFocus = "all";
        graph.autoFit = true;
      }
    }
    updateGraphFocusButton();
    updateQuickFilters();
    if (options.tab) showTab(options.tab);
    renderSelection();
    renderEventPanel();
    renderFailurePanel();
    renderStatePanel();
    if (state.selectedSeq !== null) {
      requestAnimationFrame(() => {
        const row = document.querySelector(`[data-seq="${state.selectedSeq}"]`);
        if (row && row.scrollIntoView) row.scrollIntoView({ block: "nearest" });
      });
    }
  }

  // ---------------------------------------------------------------- render

  function renderAll() {
    renderStats();
    updateQuickFilters();
    renderFailurePanel();
    renderEventPanel();
    renderStatePanel();
    renderQueryPanel();
    renderDumps();
    renderComparePanel();
    renderWaterfall();
    renderEvents();
    drawGraph();
    drawTimeline();
    renderSelection();
    const footer = $("#footer-status");
    if (footer && state.snapshot) {
      footer.textContent = `${state.snapshot.product || "blackbox"} ${
        state.snapshot.version || ""
      } · 127.0.0.1 · bounded history`;
    }
  }

  function updateQuickFilters() {
    document.querySelectorAll("#quick-filters button").forEach((button) => {
      const query = button.dataset.query || "";
      button.classList.toggle("active", query === state.query);
    });
  }

  function renderStats() {
    const snapshot = state.snapshot;
    const records = state.records;
    const errors = records.filter(
      (record) =>
        record.kind === "error" ||
        record.level === "error" ||
        record.level === "fatal",
    ).length;
    const states = records.filter((record) => record.kind === "state_change").length;
    const spans = snapshot?.waterfall ? snapshot.waterfall.length : 0;
    const ring = snapshot?.stats?.ring || {};
    const windowInfo = snapshot?.window || {};
    const dropped = (Number(windowInfo.snapshot_lost) || 0) + (Number(ring.lost) || 0);
    $("#stat-events").textContent = String(records.length);
    $("#stat-spans").textContent = String(spans);
    $("#stat-errors").textContent = String(errors);
    $("#stat-errors").className = errors > 0 ? "bad" : "";
    $("#stat-states").textContent = String(states);
    $("#stat-dropped").textContent = String(dropped);
    $("#stat-dropped").className = dropped > 0 ? "bad" : "";
    $("#stat-memory").textContent = ring.memory_bytes
      ? formatBytes(ring.memory_bytes)
      : "—";
    $("#stat-window").textContent =
      windowInfo.snapshot_first_seq && windowInfo.snapshot_last_seq
        ? `${windowInfo.snapshot_first_seq}…${windowInfo.snapshot_last_seq}`
        : state.dumpMeta
          ? `${state.dumpMeta.record_count} records`
          : "—";
    $("#stat-updated").textContent = new Date().toLocaleTimeString([], {
      hour12: false,
    });
    const meta = $("#graph-meta");
    if (meta) {
      const scope = graphScope();
      const nodeCount = scope.nodes.length;
      const edgeCount = scope.edges.length;
      const layers = snapshot?.graph?.layers || 0;
      meta.textContent = scope.focused
        ? `failure chain · ${nodeCount}/${state.nodes.length} nodes · ${layers} layers`
        : `${nodeCount} nodes · ${edgeCount} edges · ${layers} layers`;
    }
    updateGraphFocusButton();
    const waterfallMeta = $("#waterfall-meta");
    if (waterfallMeta) {
      waterfallMeta.textContent = `${snapshot?.waterfall?.length || 0} spans`;
    }
  }

  function renderFailurePanel() {
    const panel = $("#tab-failure");
    if (!panel) return;
    clear(panel);
    const failure = state.snapshot?.failure;
    if (!failure) {
      panel.appendChild(el("div", "loading", "No analysis available."));
      return;
    }
    if (!failure.found) {
      panel.appendChild(
        el("div", "summary ok", "No error, timeout or abnormal state transition in this window."),
      );
      panel.appendChild(
        el(
          "p",
          "help",
          "Blackbox marks errors, timeouts, failed spans and state changes such as " +
            "breaker.state → open as failure points.",
        ),
      );
      return;
    }
    const summary = el("div", "summary");
    summary.textContent = failure.summary;
    panel.appendChild(summary);

    panel.appendChild(el("div", "section-label", "Causal chain"));
    const chain = Array.isArray(failure.chain) ? failure.chain : [];
    const chips = el("div", "chips");
    chain.forEach((nodeId, index) => {
      const node = state.nodeById.get(String(nodeId));
      if (!node) return;
      const chip = el(
        "button",
        `chip${nodeId === failure.root_cause ? " root" : ""}`,
        `${index + 1}. ${truncate(node.label || node.name || node.kind, 34)}`,
      );
      chip.title = `${node.kind} · seq ${node.seq}`;
      chip.addEventListener("click", () => selectSeq(node.seq, { tab: "event" }));
      chips.appendChild(chip);
    });
    panel.appendChild(chips);

    panel.appendChild(el("div", "section-label", "Findings"));
    failure.findings.forEach((finding, index) => {
      const nodeId = chain[index];
      const node = nodeId === undefined ? null : state.nodeById.get(String(nodeId));
      const isRoot = nodeId === failure.root_cause;
      const row = el("div", `finding${isRoot ? " root" : ""}`);
      row.appendChild(el("span", "marker", isRoot ? "◆" : "●"));
      row.appendChild(el("span", "", finding));
      row.tabIndex = 0;
      row.addEventListener("click", () => {
        if (node) selectSeq(node.seq, { tab: "event" });
      });
      row.addEventListener("keydown", (event) => {
        if (event.key === "Enter" && node) selectSeq(node.seq, { tab: "event" });
      });
      panel.appendChild(row);
    });

    const root = state.nodeById.get(String(failure.root_cause));
    if (root) {
      panel.appendChild(el("div", "section-label", "Root cause"));
      const box = el("div", "summary");
      box.textContent = `${root.label || root.name || root.kind} · ${root.kind} · seq ${root.seq}`;
      panel.appendChild(box);
    }
  }

  function renderEventPanel() {
    const panel = $("#tab-event");
    if (!panel) return;
    clear(panel);
    const record = currentRecord();
    const node = currentNode();
    if (!record || !node) {
      panel.appendChild(
        el("div", "loading", "Select a node in the graph, timeline or a finding."),
      );
      return;
    }
    const head = el("div");
    const title = el("div", "section-label", `Event ${record.seq}`);
    head.appendChild(title);
    const chips = el("div", "chips");
    chips.appendChild(el("span", `pill ${record.level === "error" || record.level === "fatal" ? "error" : ""}`, record.level));
    chips.appendChild(el("span", "pill", record.kind));
    if (record.status && record.status !== "unset") {
      chips.appendChild(el("span", "pill warn", record.status));
    }
    head.appendChild(chips);
    panel.appendChild(head);

    const list = el("dl", "kv");
    const add = (key, value) => {
      list.appendChild(el("dt", "", key));
      list.appendChild(el("dd", "", value));
    };
    add("time", formatClock(record.time));
    add("mono", `${record.mono_ns} ns`);
    add("duration", formatDuration(record.duration_ns));
    add("trace", record.trace || "—");
    add("span", record.span);
    add("parent", record.parent || "—");
    add("correlation", record.correlation || "—");
    add("cause", record.cause_seq || "—");
    if (record.name) add("name", record.name);
    if (record.state_key) add("state key", record.state_key);
    panel.appendChild(list);

    if (record.message) {
      const msg = el("div", "summary");
      msg.textContent = record.message;
      panel.appendChild(msg);
    }

    const attrs = record.attrs && typeof record.attrs === "object" ? Object.entries(record.attrs) : [];
    if (attrs.length > 0) {
      panel.appendChild(el("div", "section-label", "Attributes"));
      const table = el("table", "attr-table");
      for (const [key, value] of attrs) {
        const tr = el("tr");
        tr.appendChild(el("td", "", key));
        tr.appendChild(el("td", "", formatValue(value)));
        table.appendChild(tr);
      }
      panel.appendChild(table);
    }

    if (record.kind === "state_change" || record.state_old !== null || record.state_new !== null) {
      panel.appendChild(el("div", "section-label", "State change"));
      const stateBox = el("div", "kv");
      stateBox.appendChild(el("dt", "", "previous"));
      stateBox.appendChild(el("dd", "code", formatValue(record.state_old)));
      stateBox.appendChild(el("dt", "", "next"));
      stateBox.appendChild(el("dd", "code", formatValue(record.state_new)));
      panel.appendChild(stateBox);
    }
    if (record.kind === "metric" || record.value !== null) {
      panel.appendChild(el("div", "section-label", "Value"));
      panel.appendChild(el("div", "code", formatValue(record.value)));
    }

    const actions = el("div", "chips");
    if (Number(record.cause_seq) > 0) {
      const cause = el("button", "chip", `Follow cause #${record.cause_seq}`);
      cause.addEventListener("click", () => selectSeq(record.cause_seq, { tab: "event" }));
      actions.appendChild(cause);
    }
    if (Number(record.parent) > 0) {
      const parentBegin = state.nodes.find(
        (candidate) =>
          candidate.kind === "span_begin" &&
          candidate.span === record.parent &&
          candidate.trace === record.trace,
      );
      if (parentBegin) {
        const button = el(
          "button",
          "chip",
          `Parent span: ${truncate(parentBegin.label, 28)}`,
        );
        button.addEventListener("click", () =>
          selectSeq(parentBegin.seq, { tab: "event" }),
        );
        actions.appendChild(button);
      }
    }
    if (actions.childElementCount > 0) {
      panel.appendChild(el("div", "section-label", "Navigate"));
      panel.appendChild(actions);
    }

    const chain = [];
    const seen = new Set();
    let cursor = record;
    for (let i = 0; i < 32 && cursor; ++i) {
      chain.push(cursor);
      const causeSeq = Number(cursor.cause_seq);
      if (!causeSeq || seen.has(causeSeq)) break;
      seen.add(causeSeq);
      const nodeId = state.seqToNode.get(causeSeq);
      cursor = nodeId === undefined ? null : state.records[nodeId];
    }
    if (chain.length > 1) {
      panel.appendChild(el("div", "section-label", "Cause chain"));
      const trail = el("div", "chips");
      chain
        .slice()
        .reverse()
        .forEach((item, index) => {
          const chip = el(
            "button",
            "chip",
            `${index + 1}. ${truncate(item.name || item.state_key || item.message || item.kind, 30)}`,
          );
          chip.addEventListener("click", () => selectSeq(item.seq, { tab: "event" }));
          trail.appendChild(chip);
        });
      panel.appendChild(trail);
    }
  }

  function renderStatePanel() {
    const panel = $("#tab-state");
    if (!panel) return;
    clear(panel);
    const histories = state.snapshot?.states || [];
    if (histories.length === 0) {
      panel.appendChild(el("div", "loading", "No state changes in this window."));
      return;
    }
    for (const history of histories) {
      const group = el("div", "state-group");
      group.appendChild(el("div", "state-key", history.key));
      for (const transition of history.transitions || []) {
        const row = el("div", "transition");
        row.dataset.seq = transition.node !== undefined && state.records[transition.node]
          ? state.records[transition.node].seq
          : "";
        row.appendChild(el("span", "time", formatClock(transition.time)));
        const body = el("span");
        body.appendChild(el("span", "code", formatValue(transition.previous)));
        body.appendChild(el("span", "arrow", " → "));
        body.appendChild(el("span", "code", formatValue(transition.next)));
        row.appendChild(body);
        row.tabIndex = 0;
        const activate = () => {
          const record = state.records[transition.node];
          if (record) selectSeq(record.seq, { tab: "event" });
        };
        row.addEventListener("click", activate);
        row.addEventListener("keydown", (event) => {
          if (event.key === "Enter") activate();
        });
        group.appendChild(row);
      }
      panel.appendChild(group);
    }
    renderSelection();
  }

  async function runComparison() {
    if (!state.compareLeft || !state.compareRight) {
      showToast("Select two dumps to compare", "error");
      return;
    }
    try {
      const url = new URL("/api/compare", location.origin);
      url.searchParams.set("left", state.compareLeft);
      url.searchParams.set("right", state.compareRight);
      const data = await requestJson(url);
      state.compareResult = data;
      state.compareError = "";
      showTab("compare");
      renderComparePanel();
      showToast("Comparison complete", "ok");
    } catch (error) {
      state.compareError = error.message;
      showTab("compare");
      renderComparePanel();
      showToast(error.message, "error");
    }
  }

  function compareSide(label, side) {
    const box = el("div", "compare-side");
    box.appendChild(el("div", "section-label", label));
    const name = el("div", "code", side.name || "—");
    box.appendChild(name);
    box.appendChild(
      el(
        "div",
        "muted",
        `${side.record_count || 0} records · ${
          side.complete ? "complete" : "recovered"
        }${side.error ? ` · ${side.error}` : ""}`,
      ),
    );
    const open = el("button", "ghost small", "Open");
    open.addEventListener("click", () => openDump(side.name));
    box.appendChild(open);
    return box;
  }

  function divergenceBox(title, diverged, detail, extra) {
    const box = el("div", diverged ? "summary" : "summary ok");
    box.appendChild(el("strong", "", title));
    box.appendChild(el("div", "", detail || (diverged ? "diverged" : "identical")));
    if (extra) box.appendChild(el("div", "muted", extra));
    return box;
  }

  function renderComparePanel() {
    const panel = $("#tab-compare");
    if (!panel) return;
    clear(panel);
    if (state.compareError) {
      panel.appendChild(el("div", "error-box", state.compareError));
      return;
    }
    const data = state.compareResult;
    if (!data) {
      panel.appendChild(
        el(
          "div",
          "loading",
          "Pick two dumps in the Dumps tab and press Compare. Event shape and state transitions are compared; timestamps and ids are ignored.",
        ),
      );
      return;
    }
    const sides = el("div", "compare-sides");
    sides.appendChild(compareSide("Left", data.left || {}));
    sides.appendChild(compareSide("Right", data.right || {}));
    panel.appendChild(sides);
    const comparison = data.comparison || {};
    panel.appendChild(el("div", "section-label", "First divergence"));
    const eventExtra = [
      data.left && comparison.left_seq ? `left seq ${comparison.left_seq}` : "",
      data.right && comparison.right_seq ? `right seq ${comparison.right_seq}` : "",
    ]
      .filter(Boolean)
      .join(" · ");
    panel.appendChild(
      divergenceBox(
        comparison.event_divergence ? "Event divergence" : "Event shape",
        Boolean(comparison.event_divergence),
        comparison.event_detail ||
          (comparison.event_divergence
            ? `at index ${comparison.event_index}`
            : "No event divergence in the common prefix."),
        eventExtra,
      ),
    );
    panel.appendChild(
      divergenceBox(
        comparison.state_divergence ? "State divergence" : "State transitions",
        Boolean(comparison.state_divergence),
        comparison.state_detail ||
          (comparison.state_divergence
            ? `key ${comparison.state_key} at index ${comparison.state_index}`
            : "No state divergence."),
        comparison.state_divergence
          ? `left ${formatValue(comparison.left_value)} · right ${formatValue(
              comparison.right_value,
            )}`
          : "",
      ),
    );
  }

  function renderQueryPanel() {
    const panel = $("#tab-query");
    if (!panel) return;
    clear(panel);
    if (state.queryHistory.length > 0) {
      panel.appendChild(el("div", "section-label", "Recent queries"));
      const recent = el("div", "chips");
      for (const query of state.queryHistory) {
        const chip = el("button", "chip", truncate(query, 54));
        chip.title = query;
        chip.addEventListener("click", () => runQuery(query));
        recent.appendChild(chip);
      }
      const clear = el("button", "chip", "Clear history");
      clear.addEventListener("click", () => {
        state.queryHistory = [];
        saveQueryHistory();
        renderQueryPanel();
      });
      recent.appendChild(clear);
      panel.appendChild(recent);
    }
    panel.appendChild(el("div", "section-label", "Current query"));
    panel.appendChild(
      el("div", "code", state.query || "(all records)"),
    );
    const info = el(
      "p",
      "help",
      `${state.records.length} records matched in the current window.`,
    );
    panel.appendChild(info);
    if (state.queryError) {
      panel.appendChild(el("div", "error-box", state.queryError));
    }

    panel.appendChild(el("div", "section-label", "Examples"));
    const chips = el("div", "chips");
    const examples = [
      "kind = error",
      "level >= warn",
      "duration > 100ms",
      "attr.http.status = 500",
      "message ~ timeout",
      "state.new = open",
      "trace ~ 0123456789abcdef",
      "NOT kind = metric",
    ];
    for (const example of examples) {
      const chip = el("button", "chip", example);
      chip.addEventListener("click", () => runQuery(example));
      chips.appendChild(chip);
    }
    panel.appendChild(chips);

    const help = el("div", "help");
    help.appendChild(el("h3", "", "Fields"));
    help.appendChild(
      el(
        "p",
        "",
        "kind, level, status, name, message, text/any, trace, span, parent, correlation, " +
          "seq, cause, duration, state, state.old, state.new, value, attr.KEY or attr[\"KEY\"].",
      ),
    );
    help.appendChild(el("h3", "", "Operators"));
    help.appendChild(
      el("p", "", "= != < <= > >= ~ (case-insensitive contains) !~ · AND OR NOT · parentheses."),
    );
    help.appendChild(el("h3", "", "Durations"));
    help.appendChild(el("p", "", "Plain integers are nanoseconds; suffixes ns, us, ms and s are accepted."));
    help.appendChild(
      el(
        "p",
        "",
        "Limits: 4096 source bytes, 1024 tokens, 512 nodes and 32 nesting levels. " +
          "Malformed queries fail explicitly; nothing is silently dropped.",
      ),
    );
    panel.appendChild(help);
  }

  function renderDumps() {
    const panel = $("#tab-dumps");
    if (!panel) return;
    clear(panel);
    const head = el("div", "chips");
    const refresh = el("button", "chip", "Refresh");
    refresh.addEventListener("click", () => loadDumps());
    const write = el("button", "chip", "Dump now");
    write.addEventListener("click", dumpNow);
    head.appendChild(refresh);
    head.appendChild(write);
    panel.appendChild(head);

    const valid = state.dumps.filter((dump) => dump.valid);
    if (valid.length >= 2) {
      panel.appendChild(el("div", "section-label", "Compare two traces"));
      const chooser = el("div", "compare-grid");
      const leftSelect = el("select");
      const rightSelect = el("select");
      for (const dump of valid) {
        leftSelect.appendChild(el("option", "", dump.name));
        rightSelect.appendChild(el("option", "", dump.name));
      }
      if (!state.compareLeft || !valid.some((dump) => dump.name === state.compareLeft)) {
        state.compareLeft = valid[0].name;
      }
      if (!state.compareRight || !valid.some((dump) => dump.name === state.compareRight)) {
        state.compareRight = valid[1].name;
      }
      leftSelect.value = state.compareLeft;
      rightSelect.value = state.compareRight;
      leftSelect.addEventListener("change", () => {
        state.compareLeft = leftSelect.value;
      });
      rightSelect.addEventListener("change", () => {
        state.compareRight = rightSelect.value;
      });
      chooser.appendChild(leftSelect);
      chooser.appendChild(rightSelect);
      const button = el("button", "primary small", "Compare");
      button.addEventListener("click", () => runComparison());
      chooser.appendChild(button);
      panel.appendChild(chooser);
    } else if (state.dumps.length > 0) {
      panel.appendChild(
        el("div", "loading", "Two valid dumps are required for comparison."),
      );
    }

    if (state.view === "dump" && state.dumpMeta) {
      panel.appendChild(el("div", "section-label", "Open dump"));
      const box = el("div", "summary ok");
      box.textContent = `${state.dumpMeta.name} · ${state.dumpMeta.record_count} records`;
      panel.appendChild(box);
    }
    if (!state.dumps.length) {
      panel.appendChild(
        el("div", "loading", "No dumps yet. Dump captures every retained record atomically."),
      );
      return;
    }
    panel.appendChild(el("div", "section-label", "Saved dumps"));
    for (const dump of state.dumps) {
      const row = el("div", "dump-row");
      const left = el("div");
      left.appendChild(el("div", "name", dump.name));
      const meta = el(
        "div",
        "meta",
        `${dump.valid ? "valid" : "invalid"} · ${dump.record_count} records · ${formatBytes(
          dump.size_bytes,
        )}${dump.created_wall_ns ? ` · ${formatClock(dump.time)}` : ""}`,
      );
      left.appendChild(meta);
      if (!dump.valid && dump.error) {
        left.appendChild(el("div", "meta", dump.error));
      }
      row.appendChild(left);
      const actions = el("div", "dump-actions");
      const open = el("button", "ghost small", "Open");
      open.disabled = !dump.valid;
      open.addEventListener("click", () => openDump(dump.name));
      const download = el("button", "ghost small", "Download");
      download.addEventListener("click", () => downloadDump(dump.name));
      actions.appendChild(open);
      actions.appendChild(download);
      row.appendChild(actions);
      panel.appendChild(row);
    }
  }

  function renderEvents() {
    const list = $("#event-list");
    if (!list) return;
    const scroll = list.scrollTop;
    clear(list);
    const records = state.records.slice().sort((a, b) => Number(b.seq) - Number(a.seq));
    const limit = 1200;
    for (const record of records.slice(0, limit)) {
      const row = el("div", "event-row");
      row.dataset.seq = record.seq;
      row.tabIndex = 0;
      row.setAttribute("role", "listitem");
      if (record.kind === "error" || record.level === "error" || record.level === "fatal") {
        row.classList.add("error");
      } else if (record.level === "warn") {
        row.classList.add("warn");
      }
      row.appendChild(el("span", "time", formatClock(record.time)));
      row.appendChild(el("span", "kind", record.kind));
      const label = el("span", "label");
      const name = record.name || record.state_key || "";
      label.appendChild(el("span", "", name ? `${name} ` : ""));
      label.appendChild(
        el(
          "span",
          "msg",
          record.message || (record.kind === "metric" ? formatValue(record.value) : ""),
        ),
      );
      row.appendChild(label);
      row.appendChild(
        el(
          "span",
          "dur",
          record.duration_ns ? formatDuration(record.duration_ns) : "",
        ),
      );
      const activate = () => selectSeq(record.seq, { tab: "event" });
      row.addEventListener("click", activate);
      row.addEventListener("keydown", (event) => {
        if (event.key === "Enter") activate();
      });
      list.appendChild(row);
    }
    list.scrollTop = scroll;
    if (records.length > limit) {
      const note = el("div", "loading", `Showing the newest ${limit} of ${records.length} records.`);
      list.appendChild(note);
    }
    renderSelection();
  }

  function renderWaterfall() {
    const container = $("#waterfall");
    if (!container) return;
    clear(container);
    const rows = state.snapshot?.waterfall || [];
    if (!rows.length) {
      container.appendChild(el("div", "loading", "No spans in this window."));
      return;
    }
    let min = Infinity;
    let max = -Infinity;
    for (const row of rows) {
      const start = Number(row.start_mono_ns) || 0;
      const end = start + Math.max(0, Number(row.duration_ns) || 0);
      min = Math.min(min, start);
      max = Math.max(max, end);
    }
    if (!Number.isFinite(min)) min = 0;
    if (!Number.isFinite(max)) max = min + 1;
    const total = Math.max(1, max - min);
    const limited = rows.slice(0, 220);
    for (const row of limited) {
      const start = Number(row.start_mono_ns) || 0;
      const duration = Math.max(0, Number(row.duration_ns) || 0);
      const line = el("div", "wf-row");
      line.dataset.seq = row.begin_node !== undefined && state.records[row.begin_node]
        ? state.records[row.begin_node].seq
        : "";
      line.tabIndex = 0;
      const name = el("div", "wf-name");
      const depth = Math.max(0, Number(row.depth) || 0);
      name.appendChild(el("span", "depth", depth ? `${"· ".repeat(depth)}` : ""));
      name.appendChild(document.createTextNode(row.name));
      if (duration > 0) {
        name.appendChild(
          el("span", "dur-inline", ` · ${formatDuration(duration)}`),
        );
      }
      name.title = `${row.name} · ${row.status} · ${formatDuration(duration)}`;
      line.appendChild(name);
      const track = el("div", "wf-track");
      const bar = el("div", "wf-bar");
      if (row.status) bar.classList.add(row.status);
      const left = ((start - min) / total) * 100;
      const width = (duration / total) * 100;
      bar.style.left = `${Math.max(0, Math.min(99.7, left))}%`;
      bar.style.width = `${Math.max(0.3, Math.min(100 - left, width))}%`;
      bar.title = `${row.name} · ${formatDuration(duration)} · ${row.status}`;
      track.appendChild(bar);
      line.appendChild(track);
      const activate = () => {
        if (row.begin_node !== undefined) {
          const record = state.records[row.begin_node];
          if (record) selectSeq(record.seq, { tab: "event" });
        }
      };
      line.addEventListener("click", activate);
      line.addEventListener("keydown", (event) => {
        if (event.key === "Enter") activate();
      });
      container.appendChild(line);
    }
    if (rows.length > limited.length) {
      container.appendChild(
        el("div", "loading", `Showing ${limited.length} of ${rows.length} spans.`),
      );
    }
    renderSelection();
  }

  function renderSelection() {
    document.querySelectorAll("[data-seq]").forEach((node) => {
      const selected =
        state.selectedSeq !== null && String(node.dataset.seq) === String(state.selectedSeq);
      node.classList.toggle("selected", selected);
    });
    drawGraph();
    drawTimeline();
  }

  // ---------------------------------------------------------------- canvas

  function setupCanvas(canvas) {
    const context = canvas.getContext("2d");
    const ratio = window.devicePixelRatio || 1;
    const width = Math.max(1, Math.floor(canvas.clientWidth));
    const height = Math.max(1, Math.floor(canvas.clientHeight));
    const pixelWidth = Math.floor(width * ratio);
    const pixelHeight = Math.floor(height * ratio);
    if (canvas.width !== pixelWidth || canvas.height !== pixelHeight) {
      canvas.width = pixelWidth;
      canvas.height = pixelHeight;
    }
    context.setTransform(ratio, 0, 0, ratio, 0, 0);
    return { context, width, height };
  }

  function nodeColor(node) {
    if (node.kind === "error" || node.level === "error" || node.level === "fatal") {
      return "#ff6b6b";
    }
    if (node.level === "warn" || node.status === "timeout") return "#f0c75e";
    if (node.kind === "state_change") return "#8f7dff";
    if (node.kind === "span_begin" || node.kind === "span_end") return "#61c8ff";
    if (node.kind === "metric") return "#4fd18b";
    return "#46566b";
  }

  function focusLayout(nodes) {
    const perRow = Math.min(5, Math.max(1, nodes.length));
    return nodes.map((node, index) => {
      const row = Math.floor(index / perRow);
      const column = index % perRow;
      const x = (row % 2 === 0 ? column : perRow - 1 - column) * 250;
      return { ...node, x, y: row * 90 };
    });
  }

  function fitGraph(nodes, width, height) {
    if (!nodes.length) return;
    let minX = Infinity;
    let minY = Infinity;
    let maxX = -Infinity;
    let maxY = -Infinity;
    for (const node of nodes) {
      minX = Math.min(minX, node.x);
      minY = Math.min(minY, node.y);
      maxX = Math.max(maxX, node.x + NODE_WIDTH);
      maxY = Math.max(maxY, node.y + NODE_HEIGHT);
    }
    minX -= 60;
    minY -= 40;
    maxX += 80;
    maxY += 50;
    const worldWidth = Math.max(1, maxX - minX);
    const worldHeight = Math.max(1, maxY - minY);
    const scale = Math.min((width - 24) / worldWidth, (height - 24) / worldHeight, 1.4);
    graph.scale = Math.max(0.08, Math.min(4, scale));
    graph.offsetX = (width - worldWidth * graph.scale) / 2 - minX * graph.scale;
    graph.offsetY = (height - worldHeight * graph.scale) / 2 - minY * graph.scale;
  }

  function drawGraph() {
    if (!graph.canvas) return;
    const { context, width, height } = setupCanvas(graph.canvas);
    graph.ctx = context;
    graph.width = width;
    graph.height = height;
    context.clearRect(0, 0, width, height);
    const scope = graphScope();
    const empty = $("#graph-empty");
    if (!scope.nodes.length) {
      if (empty) empty.classList.remove("hidden");
      graph.rects = [];
      return;
    }
    if (empty) empty.classList.add("hidden");
    const layoutNodes = scope.focused ? focusLayout(scope.nodes) : scope.nodes;
    if (graph.autoFit) fitGraph(layoutNodes, width, height);

    // subtle dot grid
    context.save();
    context.strokeStyle = "rgba(255,255,255,0.035)";
    context.lineWidth = 1;
    for (let x = (graph.offsetX % 24 + 24) % 24; x < width; x += 24) {
      context.beginPath();
      context.moveTo(x, 0);
      context.lineTo(x, height);
      context.stroke();
    }
    for (let y = (graph.offsetY % 24 + 24) % 24; y < height; y += 24) {
      context.beginPath();
      context.moveTo(0, y);
      context.lineTo(width, y);
      context.stroke();
    }
    context.restore();

    const selected = state.selectedSeq;
    const positions = new Map();
    for (const node of layoutNodes) {
      positions.set(String(node.id), {
        x: graph.offsetX + Number(node.x) * graph.scale,
        y: graph.offsetY + Number(node.y) * graph.scale,
        width: NODE_WIDTH * graph.scale,
        height: NODE_HEIGHT * graph.scale,
        node,
      });
    }

    for (const edge of scope.edges) {
      const from = positions.get(String(edge.from));
      const to = positions.get(String(edge.to));
      if (!from || !to) continue;
      const selectedEdge =
        selected !== null &&
        (Number(from.node.seq) === Number(selected) || Number(to.node.seq) === Number(selected));
      context.save();
      const colors = {
        cause: "rgba(255,122,89,0.55)",
        parent: "rgba(97,200,255,0.55)",
        span: "rgba(143,125,255,0.5)",
        correlation: "rgba(70,86,107,0.5)",
      };
      context.strokeStyle = selectedEdge
        ? "rgba(255,255,255,0.85)"
        : colors[edge.kind] || "rgba(70,86,107,0.5)";
      context.lineWidth = selectedEdge ? 1.8 : 1;
      const sx = from.x + from.width;
      const sy = from.y + from.height / 2;
      const tx = to.x;
      const ty = to.y + to.height / 2;
      const bend = Math.max(24, Math.abs(tx - sx) * 0.45);
      context.beginPath();
      context.moveTo(sx, sy);
      context.bezierCurveTo(sx + bend, sy, tx - bend, ty, tx, ty);
      context.stroke();
      // arrow head
      const angle = Math.atan2(ty - sy, tx - sx);
      context.fillStyle = context.strokeStyle;
      context.beginPath();
      context.moveTo(tx, ty);
      context.lineTo(tx - 6, ty - 4);
      context.lineTo(tx - 6, ty + 4);
      context.closePath();
      context.fill();
      context.restore();
    }

    graph.rects = [];
    for (const node of layoutNodes) {
      const screen = positions.get(String(node.id));
      if (!screen) continue;
      const isSelected = selected !== null && Number(node.seq) === Number(selected);
      const isRoot = graph.rootCauseId !== null && Number(node.id) === Number(graph.rootCauseId);
      const color = nodeColor(node);
      context.save();
      if (isRoot) {
        context.shadowColor = "rgba(255,107,107,0.75)";
        context.shadowBlur = 16;
      }
      context.fillStyle = isSelected ? "rgba(97,200,255,0.2)" : "rgba(20,28,40,0.96)";
      context.strokeStyle = isSelected ? "#e6f6ff" : color;
      context.lineWidth = isSelected ? 2 : 1.2;
      const radius = Math.max(3, 7 * graph.scale);
      roundRect(context, screen.x, screen.y, screen.width, screen.height, radius);
      context.fill();
      context.stroke();
      context.restore();

      if (graph.scale > 0.28) {
        const padding = Math.max(4, 10 * graph.scale);
        context.save();
        context.beginPath();
        context.rect(screen.x, screen.y, screen.width, screen.height);
        context.clip();
        context.fillStyle = isSelected ? "#f2fbff" : "#c9d6e6";
        const fontSize = Math.max(8, 11 * graph.scale);
        context.font = `${fontSize}px ui-monospace, Menlo, monospace`;
        context.textBaseline = "middle";
        const available = screen.width - padding * 2 - 10 * graph.scale;
        const maxCharacters = Math.max(
          3,
          Math.floor(available / (fontSize * 0.62)),
        );
        const label = truncate(node.label || node.name || node.kind, maxCharacters);
        context.fillText(label, screen.x + padding, screen.y + screen.height / 2);
        context.restore();
        context.save();
        context.fillStyle = color;
        context.globalAlpha = 0.9;
        context.beginPath();
        context.arc(screen.x + screen.width - 8 * graph.scale, screen.y + 8 * graph.scale, 2.2 * graph.scale, 0, Math.PI * 2);
        context.fill();
        context.restore();
      }
      graph.rects.push({
        x: screen.x,
        y: screen.y,
        width: screen.width,
        height: screen.height,
        seq: Number(node.seq),
        node,
      });
    }
  }

  function roundRect(context, x, y, width, height, radius) {
    const r = Math.min(radius, width / 2, height / 2);
    context.beginPath();
    context.moveTo(x + r, y);
    context.arcTo(x + width, y, x + width, y + height, r);
    context.arcTo(x + width, y + height, x, y + height, r);
    context.arcTo(x, y + height, x, y, r);
    context.arcTo(x, y, x + width, y, r);
    context.closePath();
  }

  function graphHit(x, y) {
    for (let i = graph.rects.length - 1; i >= 0; --i) {
      const rect = graph.rects[i];
      if (x >= rect.x && x <= rect.x + rect.width && y >= rect.y && y <= rect.y + rect.height) {
        return rect;
      }
    }
    return null;
  }

  function bindGraph() {
    const canvas = graph.canvas;
    if (!canvas) return;
    const local = (event) => {
      const rect = canvas.getBoundingClientRect();
      return { x: event.clientX - rect.left, y: event.clientY - rect.top };
    };
    canvas.addEventListener("pointerdown", (event) => {
      const point = local(event);
      graph.dragging = true;
      graph.dragMoved = false;
      graph.dragStartX = point.x;
      graph.dragStartY = point.y;
      graph.startOffsetX = graph.offsetX;
      graph.startOffsetY = graph.offsetY;
      canvas.setPointerCapture(event.pointerId);
      canvas.classList.add("dragging");
    });
    canvas.addEventListener("pointermove", (event) => {
      const point = local(event);
      if (graph.dragging) {
        const dx = point.x - graph.dragStartX;
        const dy = point.y - graph.dragStartY;
        if (Math.abs(dx) + Math.abs(dy) > 3) graph.dragMoved = true;
        graph.offsetX = graph.startOffsetX + dx;
        graph.offsetY = graph.startOffsetY + dy;
        graph.autoFit = false;
        drawGraph();
      }
      const hit = graphHit(point.x, point.y);
      const tooltip = $("#graph-tooltip");
      if (tooltip) {
        if (hit && !graph.dragMoved) {
          tooltip.textContent = `${hit.node.label || hit.node.kind}\n${hit.node.kind} · seq ${hit.node.seq} · ${hit.node.status}`;
          tooltip.style.left = `${Math.min(point.x + 14, graph.width - 240)}px`;
          tooltip.style.top = `${Math.min(point.y + 14, graph.height - 60)}px`;
          tooltip.classList.remove("hidden");
        } else {
          tooltip.classList.add("hidden");
        }
      }
    });
    const finish = (event) => {
      if (!graph.dragging) return;
      graph.dragging = false;
      canvas.classList.remove("dragging");
      canvas.releasePointerCapture?.(event.pointerId);
      if (!graph.dragMoved) {
        const point = local(event);
        const hit = graphHit(point.x, point.y);
        if (hit) selectSeq(hit.seq, { tab: "event" });
      }
      const tooltip = $("#graph-tooltip");
      if (tooltip) tooltip.classList.add("hidden");
    };
    canvas.addEventListener("pointerup", finish);
    canvas.addEventListener("pointercancel", finish);
    canvas.addEventListener("pointerleave", () => {
      $("#graph-tooltip")?.classList.add("hidden");
    });
    canvas.addEventListener(
      "wheel",
      (event) => {
        event.preventDefault();
        const point = local(event);
        const worldX = (point.x - graph.offsetX) / graph.scale;
        const worldY = (point.y - graph.offsetY) / graph.scale;
        const factor = Math.exp(-event.deltaY * 0.0015);
        graph.scale = Math.max(0.08, Math.min(4, graph.scale * factor));
        graph.offsetX = point.x - worldX * graph.scale;
        graph.offsetY = point.y - worldY * graph.scale;
        graph.autoFit = false;
        drawGraph();
      },
      { passive: false },
    );
    canvas.addEventListener("dblclick", () => {
      graph.autoFit = true;
      drawGraph();
    });
  }

  function drawTimeline() {
    if (!timeline.canvas) return;
    const { context, width, height } = setupCanvas(timeline.canvas);
    timeline.ctx = context;
    timeline.width = width;
    timeline.height = height;
    context.clearRect(0, 0, width, height);
    timeline.hits = [];
    const records = state.records;
    if (!records.length) {
      context.fillStyle = "#5d6a7c";
      context.font = "12px ui-monospace, Menlo, monospace";
      context.fillText("No events in this window", 16, height / 2);
      return;
    }
    let min = Infinity;
    let max = -Infinity;
    for (const record of records) {
      const value = Number(record.mono_ns) || 0;
      min = Math.min(min, value);
      max = Math.max(max, value);
    }
    if (!Number.isFinite(min)) min = 0;
    if (max <= min) max = min + 1;
    const left = 56;
    const right = width - 16;
    const span = Math.max(1, right - left);
    const xOf = (value) => left + ((Number(value) - min) / (max - min)) * span;

    const lanes = [
      { y: 20, label: "spans", color: "#61c8ff" },
      { y: 45, label: "errors", color: "#ff6b6b" },
      { y: 67, label: "state", color: "#8f7dff" },
      { y: 88, label: "log", color: "#8795a8" },
    ];
    context.font = "9px ui-monospace, Menlo, monospace";
    context.textBaseline = "middle";
    for (const lane of lanes) {
      context.fillStyle = "rgba(255,255,255,0.03)";
      context.fillRect(left, lane.y - 8, span, 16);
      context.fillStyle = "#5d6a7c";
      context.fillText(lane.label, 8, lane.y);
    }
    // time ruler
    context.strokeStyle = "rgba(255,255,255,0.05)";
    context.fillStyle = "#5d6a7c";
    for (let i = 0; i <= 5; ++i) {
      const x = left + (span * i) / 5;
      context.beginPath();
      context.moveTo(x, 2);
      context.lineTo(x, height - 2);
      context.stroke();
      const rel = ((max - min) * i) / 5;
      context.fillText(`+${formatRelative(rel)}`, Math.min(x + 3, width - 44), 8);
    }

    const waterfall = state.snapshot?.waterfall || [];
    for (const row of waterfall) {
      const start = xOf(row.start_mono_ns);
      const duration = Math.max(0, Number(row.duration_ns) || 0);
      const end = xOf(Number(row.start_mono_ns) + duration);
      const widthPx = Math.max(2, end - start);
      const selected = state.selectedSeq !== null && state.records[row.begin_node] &&
        Number(state.records[row.begin_node].seq) === Number(state.selectedSeq);
      context.fillStyle = selected
        ? "rgba(255,255,255,0.85)"
        : row.status === "error" || row.status === "timeout"
          ? "rgba(255,107,107,0.55)"
          : "rgba(97,200,255,0.45)";
      context.fillRect(start, lanes[0].y - 5, Math.min(widthPx, right - start), 10);
      if (row.begin_node !== undefined && state.records[row.begin_node]) {
        timeline.hits.push({
          x: start,
          y: lanes[0].y,
          seq: state.records[row.begin_node].seq,
        });
      }
    }

    for (const record of records) {
      const x = xOf(record.mono_ns);
      const isError = record.kind === "error" || record.level === "error" || record.level === "fatal";
      const isState = record.kind === "state_change";
      const isMetric = record.kind === "metric";
      const lane = isError ? lanes[1] : isState ? lanes[2] : lanes[3];
      const selected = state.selectedSeq !== null && Number(record.seq) === Number(state.selectedSeq);
      context.fillStyle = selected ? "#ffffff" : lane.color;
      context.beginPath();
      if (isState) {
        context.moveTo(x, lane.y - 4);
        context.lineTo(x + 4, lane.y);
        context.lineTo(x, lane.y + 4);
        context.lineTo(x - 4, lane.y);
        context.closePath();
      } else {
        context.arc(x, lane.y, selected ? 3.6 : isMetric ? 2 : 2.6, 0, Math.PI * 2);
      }
      context.fill();
      timeline.hits.push({ x, y: lane.y, seq: record.seq });
    }

    if (state.selectedSeq !== null) {
      const record = currentRecord();
      if (record) {
        const x = xOf(record.mono_ns);
        context.strokeStyle = "rgba(255,255,255,0.55)";
        context.setLineDash([3, 4]);
        context.beginPath();
        context.moveTo(x, 0);
        context.lineTo(x, height);
        context.stroke();
        context.setLineDash([]);
      }
    }
  }

  function bindTimeline() {
    const canvas = timeline.canvas;
    if (!canvas) return;
    canvas.addEventListener("click", (event) => {
      const rect = canvas.getBoundingClientRect();
      const x = event.clientX - rect.left;
      const y = event.clientY - rect.top;
      let best = null;
      let bestDistance = 14;
      for (const hit of timeline.hits) {
        const distance = Math.abs(hit.x - x) + Math.abs(hit.y - y) * 0.35;
        if (distance < bestDistance) {
          bestDistance = distance;
          best = hit;
        }
      }
      if (best) selectSeq(best.seq, { tab: "event" });
    });
  }

  // ---------------------------------------------------------------- queries

  function markQueryInvalid(invalid) {
    const input = $("#query-input");
    if (input) input.classList.toggle("invalid", Boolean(invalid));
  }

  async function runQuery(text) {
    const input = $("#query-input");
    const query = String(text ?? "").trim();
    if (input) input.value = query;
    state.query = query;
    state.queryError = "";
    markQueryInvalid(false);
    state.graphFocus = "failure";
    graph.autoFit = true;
    stopStream();
    const loaded = await loadSnapshot();
    if (loaded) {
      rememberQuery(query);
      renderQueryPanel();
      startStream();
      showTab("failure");
    }
  }

  async function clearQuery() {
    const input = $("#query-input");
    if (input) input.value = "";
    state.query = "";
    state.queryError = "";
    markQueryInvalid(false);
    graph.autoFit = true;
    stopStream();
    const loaded = await loadSnapshot();
    if (loaded) startStream();
  }

  // ---------------------------------------------------------------- stream

  function stopStream() {
    if (state.stream) {
      state.stream.close();
      state.stream = null;
    }
    stopPolling();
  }

  function startPolling() {
    if (state.poll) return;
    state.poll = setInterval(() => {
      if (state.view === "live") loadSnapshot({ silent: true });
    }, 5000);
  }

  function stopPolling() {
    if (state.poll) {
      clearInterval(state.poll);
      state.poll = null;
    }
  }

  function startStream() {
    stopStream();
    if (state.view !== "live" || !state.token) return;
    const url = new URL("/api/stream", location.origin);
    url.searchParams.set("token", state.token);
    url.searchParams.set("minutes", String(state.minutes));
    if (state.query) url.searchParams.set("q", state.query);
    setLive("connecting", "");
    const stream = new EventSource(url.toString());
    state.stream = stream;
    stream.addEventListener("open", () => {
      setLive("live", "on");
      stopPolling();
    });
    stream.addEventListener("snapshot", (event) => {
      if (state.view !== "live") return;
      try {
        const data = JSON.parse(event.data);
        applySnapshot(data, { quiet: true });
        setLive("live", "on");
      } catch (error) {
        setLive("error", "off");
      }
    });
    stream.addEventListener("error", (event) => {
      if (event && event.data) {
        try {
          const data = JSON.parse(event.data);
          showToast(data.error || "Stream error", "error");
        } catch {
          showToast("Stream error", "error");
        }
        return;
      }
      setLive("reconnecting", "off");
      startPolling();
    });
  }

  // ---------------------------------------------------------------- tabs

  function showTab(name) {
    state.activeTab = name;
    document.querySelectorAll(".tab").forEach((button) => {
      button.classList.toggle("active", button.dataset.tab === name);
    });
    document.querySelectorAll(".tab-body").forEach((body) => {
      body.classList.toggle("hidden", body.id !== `tab-${name}`);
    });
    if (name === "dumps") loadDumps({ silent: true });
    if (name === "compare") renderComparePanel();
  }

  function bindTabs() {
    document.querySelectorAll(".tab").forEach((button) => {
      button.addEventListener("click", () => showTab(button.dataset.tab));
    });
  }

  // ---------------------------------------------------------------- init

  function bindControls() {
    $("#query-form")?.addEventListener("submit", (event) => {
      event.preventDefault();
      runQuery($("#query-input")?.value || "");
    });
    $("#query-clear")?.addEventListener("click", clearQuery);
    $("#minutes")?.addEventListener("change", async (event) => {
      state.minutes = Number(event.target.value) || 0;
      graph.autoFit = true;
      stopStream();
      const loaded = await loadSnapshot();
      if (loaded) startStream();
    });
    $("#refresh")?.addEventListener("click", () => {
      loadSnapshot();
      loadDumps({ silent: true });
    });
    $("#dump-now")?.addEventListener("click", dumpNow);
    $("#demo-trigger")?.addEventListener("click", triggerDemo);
    document.querySelectorAll("#quick-filters button").forEach((button) => {
      button.addEventListener("click", () => runQuery(button.dataset.query || ""));
    });
    $("#live-button")?.addEventListener("click", goLive);
    $("#graph-fit")?.addEventListener("click", () => {
      graph.autoFit = true;
      drawGraph();
    });
    $("#graph-focus")?.addEventListener("click", () => {
      const failure = state.snapshot?.failure;
      if (state.graphFocus === "failure" && (!failure || !failure.found)) {
        state.graphFocus = "all";
      } else {
        state.graphFocus = state.graphFocus === "failure" ? "all" : "failure";
      }
      graph.autoFit = true;
      updateGraphFocusButton();
      drawGraph();
      renderStats();
    });

    document.addEventListener("keydown", (event) => {
      const tag = event.target && event.target.tagName;
      const typing = tag === "INPUT" || tag === "SELECT" || tag === "TEXTAREA";
      if (event.key === "/" && !typing) {
        event.preventDefault();
        $("#query-input")?.focus();
      } else if (event.key === "Escape") {
        if (typing && event.target) event.target.blur();
        selectSeq(null);
      } else if ((event.key === "r" || event.key === "R") && !typing) {
        loadSnapshot();
        loadDumps({ silent: true });
      } else if ((event.key === "d" || event.key === "D") && !typing) {
        dumpNow();
      }
    });

    const observer = new ResizeObserver(() => {
      drawGraph();
      drawTimeline();
    });
    const graphWrap = graph.canvas?.parentElement;
    const timelineWrap = timeline.canvas?.parentElement;
    if (graphWrap) observer.observe(graphWrap);
    if (timelineWrap) observer.observe(timelineWrap);
  }

  async function init() {
    graph.canvas = $("#graph-canvas");
    timeline.canvas = $("#timeline-canvas");
    bindTabs();
    bindGraph();
    bindTimeline();
    bindControls();

    const params = new URLSearchParams(location.search);
    state.token = params.get("token") || readCookie("bb_token");
    state.queryHistory = loadQueryHistory();
    const initialDump = params.get("dump");
    const initialQuery = params.get("q");
    if (initialQuery) {
      state.query = initialQuery;
      const input = $("#query-input");
      if (input) input.value = initialQuery;
    }
    const selectedMinutes = $("#minutes");
    if (selectedMinutes) state.minutes = Number(selectedMinutes.value) || 0;

    if (!state.token) {
      setLive("no token", "off");
      showToast(
        "Open the URL printed by blackbox_launcher (it contains the session token).",
        "error",
      );
      return;
    }
    try {
      if (initialDump) {
        await openDump(initialDump);
        await loadDumps({ silent: true });
        return;
      }
      const loaded = await loadSnapshot();
      await loadDumps({ silent: true });
      if (loaded) startStream();
      else setLive("error", "off");
    } catch (error) {
      setLive("error", "off");
      showToast(error.message, "error");
    }
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init, { once: true });
  } else {
    init();
  }
})();
