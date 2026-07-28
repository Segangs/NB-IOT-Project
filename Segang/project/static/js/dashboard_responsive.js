(function (root, factory) {
    const api = factory();
    if (typeof module === "object" && module.exports) {
        module.exports = api;
    } else {
        root.NBResponsive = api;
    }
}(typeof globalThis !== "undefined" ? globalThis : this, function () {
    "use strict";

    const MOBILE_QUERY = "(max-width: 768px)";
    const OPEN_CLASS = "mobile-sidebar-open";

    function selectChartWindow(labels, values, mobile, limit = 10) {
        if (!Array.isArray(labels) || !Array.isArray(values)) {
            throw new TypeError("labels and values must be arrays");
        }
        if (labels.length !== values.length) {
            throw new RangeError("labels and values must have the same length");
        }
        if (!Number.isInteger(limit) || limit < 1) {
            throw new RangeError("limit must be a positive integer");
        }

        const start = mobile ? Math.max(0, labels.length - limit) : 0;
        return {
            labels: labels.slice(start),
            values: values.slice(start),
        };
    }

    function installMobileSidebar(documentObject, windowObject) {
        const button = documentObject.getElementById("mobileMenuButton");
        const sidebar = documentObject.getElementById("sidebar");
        const overlay = documentObject.getElementById("sidebarOverlay");

        if (!button || !sidebar || !overlay || !documentObject.body) {
            return null;
        }

        const mediaQuery = windowObject.matchMedia(MOBILE_QUERY);
        const navLinks = Array.from(sidebar.querySelectorAll("a"));

        function isMobile() {
            return Boolean(mediaQuery.matches);
        }

        function setOpen(open) {
            const shouldOpen = isMobile() && Boolean(open);
            documentObject.body.classList.toggle(OPEN_CLASS, shouldOpen);
            button.setAttribute("aria-expanded", shouldOpen ? "true" : "false");
            sidebar.setAttribute("aria-hidden", isMobile() && !shouldOpen ? "true" : "false");
        }

        function open() {
            setOpen(true);
        }

        function close() {
            setOpen(false);
        }

        function toggle() {
            setOpen(!documentObject.body.classList.contains(OPEN_CLASS));
        }

        function handleKeydown(event) {
            if (event.key === "Escape") {
                close();
            }
        }

        function handleViewportChange() {
            close();
        }

        button.addEventListener("click", toggle);
        overlay.addEventListener("click", close);
        documentObject.addEventListener("keydown", handleKeydown);
        windowObject.addEventListener("resize", handleViewportChange);
        navLinks.forEach((link) => link.addEventListener("click", close));
        setOpen(false);

        return {
            close,
            destroy() {
                button.removeEventListener("click", toggle);
                overlay.removeEventListener("click", close);
                documentObject.removeEventListener("keydown", handleKeydown);
                windowObject.removeEventListener("resize", handleViewportChange);
                navLinks.forEach((link) => link.removeEventListener("click", close));
                close();
            },
            open,
        };
    }

    return {
        installMobileSidebar,
        selectChartWindow,
    };
}));
