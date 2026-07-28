import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

import dashboardResponsive from "../static/js/dashboard_responsive.js";

const {
    installMobileSidebar,
    selectChartWindow,
} = dashboardResponsive;

const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const templatesRoot = path.join(projectRoot, "templates");
const zoomGuardHref = "/static/css/mobile_input_zoom_guard.css";


function readProjectFile(...parts) {
    return fs.readFileSync(path.join(projectRoot, ...parts), "utf8");
}


class FakeClassList {
    constructor() {
        this.values = new Set();
    }

    contains(name) {
        return this.values.has(name);
    }

    toggle(name, force) {
        if (force === undefined) {
            force = !this.values.has(name);
        }
        if (force) {
            this.values.add(name);
        } else {
            this.values.delete(name);
        }
        return force;
    }
}


class FakeEventTarget {
    constructor() {
        this.listeners = new Map();
        this.attributes = new Map();
    }

    addEventListener(name, listener) {
        const listeners = this.listeners.get(name) ?? [];
        listeners.push(listener);
        this.listeners.set(name, listeners);
    }

    removeEventListener(name, listener) {
        const listeners = this.listeners.get(name) ?? [];
        this.listeners.set(name, listeners.filter((candidate) => candidate !== listener));
    }

    dispatch(name, event = {}) {
        for (const listener of this.listeners.get(name) ?? []) {
            listener({ type: name, ...event });
        }
    }

    setAttribute(name, value) {
        this.attributes.set(name, String(value));
    }

    getAttribute(name) {
        return this.attributes.get(name);
    }
}


function makeSidebarFixture() {
    const button = new FakeEventTarget();
    const overlay = new FakeEventTarget();
    const sidebar = new FakeEventTarget();
    const firstLink = new FakeEventTarget();
    const secondLink = new FakeEventTarget();
    sidebar.querySelectorAll = () => [firstLink, secondLink];

    const body = { classList: new FakeClassList() };
    const documentTarget = new FakeEventTarget();
    documentTarget.body = body;
    documentTarget.getElementById = (id) => ({
        mobileMenuButton: button,
        sidebar,
        sidebarOverlay: overlay,
    })[id] ?? null;

    const windowTarget = new FakeEventTarget();
    windowTarget.matchMedia = () => ({ matches: true });

    return {
        body,
        button,
        documentTarget,
        firstLink,
        overlay,
        secondLink,
        sidebar,
        windowTarget,
    };
}


test("mobile chart window keeps only the newest ten aligned points", () => {
    const labels = Array.from({ length: 50 }, (_, index) => `L${index + 1}`);
    const values = Array.from({ length: 50 }, (_, index) => index + 1);

    assert.deepEqual(
        selectChartWindow(labels, values, true),
        {
            labels: ["L41", "L42", "L43", "L44", "L45", "L46", "L47", "L48", "L49", "L50"],
            values: [41, 42, 43, 44, 45, 46, 47, 48, 49, 50],
        },
    );
});


test("desktop chart window preserves all fifty points", () => {
    const labels = Array.from({ length: 50 }, (_, index) => `L${index + 1}`);
    const values = Array.from({ length: 50 }, (_, index) => index + 1);

    const selected = selectChartWindow(labels, values, false);

    assert.deepEqual(selected.labels, labels);
    assert.deepEqual(selected.values, values);
    assert.notEqual(selected.labels, labels);
    assert.notEqual(selected.values, values);
});


test("chart window rejects misaligned input instead of silently pairing wrong values", () => {
    assert.throws(
        () => selectChartWindow(["09:00", "09:10"], [1], true),
        /same length/i,
    );
});


test("mobile sidebar opens from the button and closes through every escape route", () => {
    const fixture = makeSidebarFixture();
    const controller = installMobileSidebar(fixture.documentTarget, fixture.windowTarget);

    assert.equal(fixture.button.getAttribute("aria-expanded"), "false");
    assert.equal(fixture.sidebar.getAttribute("aria-hidden"), "true");

    fixture.button.dispatch("click");
    assert.equal(fixture.body.classList.contains("mobile-sidebar-open"), true);
    assert.equal(fixture.button.getAttribute("aria-expanded"), "true");
    assert.equal(fixture.sidebar.getAttribute("aria-hidden"), "false");

    fixture.overlay.dispatch("click");
    assert.equal(fixture.body.classList.contains("mobile-sidebar-open"), false);

    fixture.button.dispatch("click");
    fixture.documentTarget.dispatch("keydown", { key: "Escape" });
    assert.equal(fixture.body.classList.contains("mobile-sidebar-open"), false);

    fixture.button.dispatch("click");
    fixture.firstLink.dispatch("click");
    assert.equal(fixture.body.classList.contains("mobile-sidebar-open"), false);

    fixture.button.dispatch("click");
    fixture.secondLink.dispatch("click");
    assert.equal(fixture.body.classList.contains("mobile-sidebar-open"), false);

    controller.destroy();
});


test("every standalone page loads the mobile input zoom guard", () => {
    const templateNames = fs.readdirSync(templatesRoot)
        .filter((name) => name.endsWith(".html"))
        .sort();

    for (const templateName of templateNames) {
        const html = readProjectFile("templates", templateName);
        const extendsLayout = /\{%\s*extends\s+["']layout\.html["']\s*%\}/.test(html);

        if (extendsLayout) {
            continue;
        }

        assert.match(
            html,
            /<meta\s+name=["']viewport["'][^>]*width=device-width[^>]*>/i,
            `${templateName} must declare the standard mobile viewport`,
        );
        assert.ok(
            html.includes(`href="${zoomGuardHref}"`),
            `${templateName} must load ${zoomGuardHref}`,
        );
    }
});


test("mobile zoom guard uses 16px controls without disabling user zoom", () => {
    const css = readProjectFile("static", "css", "mobile_input_zoom_guard.css");
    const templateNames = fs.readdirSync(templatesRoot)
        .filter((name) => name.endsWith(".html"));

    assert.match(css, /@media[^{]*max-width:\s*768px/i);
    assert.match(css, /\binput\b/i);
    assert.match(css, /\bselect\b/i);
    assert.match(css, /\btextarea\b/i);
    assert.match(css, /font-size:\s*16px\s*!important/i);

    for (const templateName of templateNames) {
        const html = readProjectFile("templates", templateName);
        assert.doesNotMatch(
            html,
            /user-scalable\s*=\s*no|maximum-scale\s*=\s*1(?:\.0)?/i,
            `${templateName} must preserve manual zoom`,
        );
    }
});
