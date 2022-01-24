// Mouse Targets Overview
//
// Mouse target data is used to figure out which element to highlight when the
// mouse is hovered/clicked on different parts of the screen when the element
// picker is used. To determine this, we need to know the bounding client rects
// of every element (easy) and the order in which different elements are stacked
// (not easy).
//
// To figure out the order in which elements are stacked, we reconstruct the
// stacking contexts on the page and the order in which elements are laid out
// within those stacking contexts, allowing us to assemble a sorted array of
// elements such that for any two elements that overlap, the frontmost element
// appears first in the array.
//
// References:
//
// https://www.w3.org/TR/CSS21/zindex.html
//
//   We try to follow this reference, although not all of its rules are
//   implemented yet.
//
// https://developer.mozilla.org/en-US/docs/Web/CSS/CSS_Positioning/Understanding_z_index/The_stacking_context
//
//   This is helpful but the rules for when stacking contexts are created are
//   quite baroque and don't seem to match up with the spec above, so they are
//   mostly ignored here.

function assert(v, msg = "") {
  if (!v) {
    log(`Error: Assertion failed ${msg} ${Error().stack}`);
    throw new Error("Assertion failed!");
  }
}

// Information about an element needed to add it to a stacking context.
function StackingContextElement(node, parent, offset, style, clipBounds) {
  assert(node.nodeType == Node.ELEMENT_NODE);

  // Underlying element.
  this.raw = node;

  // Offset relative to the outer window of the window containing this context.
  this.offset = offset;

  // the parent StackingContextElement
  this.parent = parent;

  // Style and clipping information for the node.
  this.style = style;
  this.clipBounds = clipBounds;

  // Any stacking context at which this element is the root.
  this.context = null;
}

StackingContextElement.prototype = {
  isPositioned() {
    return this.style.getPropertyValue("position") != "static";
  },

  isAbsolutelyPositioned() {
    return ["absolute", "fixed"].includes(this.style.getPropertyValue("position"));
  },

  isTable() {
    return ["table", "inline-table"].includes(this.style.getPropertyValue("display"));
  },

  isFlexOrGridContainer() {
    return ["flex", "inline-flex", "grid", "inline-grid"].includes(
      this.style.getPropertyValue("display")
    );
  },

  isBlockElement() {
    return ["block", "table", "flex", "grid"].includes(this.style.getPropertyValue("display"));
  },

  isFloat() {
    return this.style.getPropertyValue("float") != "none";
  },

  getPositionedAncestor() {
    if (this.isPositioned()) {
      return this;
    }
    return this.parent?.getPositionedAncestor();
  },

  // see https://developer.mozilla.org/en-US/docs/Web/Guide/CSS/Block_formatting_context
  getFormattingContextElement() {
    if (!this.parent) {
      return this;
    }
    if (this.isFloat()) {
      return this;
    }
    if (this.isAbsolutelyPositioned()) {
      return this;
    }
    if (
      [
        "inline-block",
        "table-cell",
        "table-caption",
        "table",
        "table-row",
        "table-row-group",
        "table-header-group",
        "table-footer-group",
        "inline-table",
        "flow-root",
      ].includes(this.style.getPropertyValue("display"))
    ) {
      return this;
    }
    if (
      this.isBlockElement() &&
      !(
        ["visible", "clip"].includes(this.style.getPropertyValue("overflow-x")) &&
        ["visible", "clip"].includes(this.style.getPropertyValue("overflow-y"))
      )
    ) {
      return this;
    }
    if (["layout", "content", "paint"].includes(this.style.getPropertyValue("contain"))) {
      return this;
    }
    if (this.parent.isFlexOrGridContainer() && !this.isFlexOrGridContainer() && !this.isTable()) {
      return this;
    }
    if (
      this.style.getPropertyValue("column-count") != "auto" ||
      this.style.getPropertyValue("column-width") != "auto"
    ) {
      return this;
    }
    if (this.style.getPropertyValue("column-span") == "all") {
      return this;
    }
    return this.parent.getFormattingContextElement();
  },

  // toString() {
  //   return getObjectIdRaw(this.raw);
  // },
};

let gNextStackingContextId = 1;

// Information about all the nodes in the same stacking context.
// The spec says that some elements should be treated as if they
// "created a new stacking context, but any positioned descendants and
// descendants which actually create a new stacking context should be
// considered part of the parent stacking context, not this new one".
// For these elements we also create a StackingContext but pass the
// parent stacking context to the constructor as the "realStackingContext".
function StackingContext(window, root, offset, realStackingContext) {
  this.window = window;
  this.id = gNextStackingContextId++;

  this.realStackingContext = realStackingContext || this;

  // Offset relative to the outer window of the window containing this context.
  this.offset = offset || { left: 0, top: 0 };

  // The arrays below are filled in tree order (preorder depth first traversal).

  // All non-positioned, non-floating elements.
  this.nonPositionedElements = [];

  // All floating elements.
  this.floatingElements = [];

  // All positioned elements with an auto or zero z-index.
  this.positionedElements = [];

  // Arrays of elements with non-zero z-indexes, indexed by that z-index.
  this.zIndexElements = new Map();

  this.root = root;
  if (root) {
    this.addChildrenWithParent(root);
  }
}

StackingContext.prototype = {
  toString() {
    return `StackingContext:${this.id}`;
  },

  // Add node and its descendants to this stacking context.
  add(node, parentElem, offset) {
    const style = this.window.getComputedStyle(node);
    const position = style.getPropertyValue("position");
    let clipBounds;
    if (position == "absolute") {
      clipBounds = parentElem?.getPositionedAncestor()?.clipBounds || {};
    } else if (position == "fixed") {
      clipBounds = {};
    } else {
      clipBounds = parentElem?.clipBounds || {};
    }
    clipBounds = Object.assign({}, clipBounds);
    const elem = new StackingContextElement(node, parentElem, offset, style, clipBounds);
    if (!["HTML", "BODY"].includes(elem.raw.tagName)) {
      if (style.getPropertyValue("overflow-x") != "visible") {
        const clipBounds2 = elem.getFormattingContextElement().raw.getBoundingClientRect();
        elem.clipBounds.left =
          clipBounds.left !== undefined
            ? Math.max(clipBounds2.left, clipBounds.left)
            : clipBounds2.left;
        elem.clipBounds.right =
          clipBounds.right !== undefined
            ? Math.min(clipBounds2.right, clipBounds.right)
            : clipBounds2.right;
      }
      if (style.getPropertyValue("overflow-y") != "visible") {
        const clipBounds2 = elem.getFormattingContextElement().raw.getBoundingClientRect();
        elem.clipBounds.top =
          clipBounds.top !== undefined
            ? Math.max(clipBounds2.top, clipBounds.top)
            : clipBounds2.top;
        elem.clipBounds.bottom =
          clipBounds.bottom !== undefined
            ? Math.min(clipBounds2.bottom, clipBounds.bottom)
            : clipBounds2.bottom;
      }
    }

    // Create a new stacking context for any iframes.
    if (elem.raw.tagName == "IFRAME") {
      const { left, top } = elem.raw.getBoundingClientRect();
      this.addContext(elem, undefined, left, top);
      elem.context.addChildren(elem.raw.contentWindow.document);
    }

    if (!elem.style) {
      this.addNonPositionedElement(elem);
      this.addChildrenWithParent(elem);
      return;
    }

    const parentDisplay = elem.parent?.style?.getPropertyValue("display");
    if (
      position != "static" ||
      ["flex", "inline-flex", "grid", "inline-grid"].includes(parentDisplay)
    ) {
      const zIndex = elem.style.getPropertyValue("z-index");
      if (zIndex != "auto") {
        this.addContext(elem);
        // Elements with a zero z-index have their own stacking context but are
        // grouped with other positioned children with an auto z-index.
        const index = +zIndex | 0;
        if (index) {
          this.realStackingContext.addZIndexElement(elem, index);
          return;
        }
      }

      if (position != "static") {
        this.realStackingContext.addPositionedElement(elem);
        if (!elem.context) {
          this.addContext(elem, this.realStackingContext);
        }
      } else {
        this.addNonPositionedElement(elem);
        if (!elem.context) {
          this.addChildrenWithParent(elem);
        }
      }
      return;
    }

    if (elem.isFloat()) {
      // Group the element and its descendants.
      this.addContext(elem, this.realStackingContext);
      this.addFloatingElement(elem);
      return;
    }

    const display = elem.style.getPropertyValue("display");
    if (display == "inline-block" || display == "inline-table") {
      // Group the element and its descendants.
      this.addContext(elem, this.realStackingContext);
      this.addNonPositionedElement(elem);
      return;
    }

    this.addNonPositionedElement(elem);
    this.addChildrenWithParent(elem);
  },

  addContext(elem, realStackingContext, left = 0, top = 0) {
    if (elem.context) {
      assert(!left && !top);
      return;
    }
    const offset = {
      left: this.offset.left + left,
      top: this.offset.top + top,
    };
    elem.context = new StackingContext(this.window, elem, offset, realStackingContext);
  },

  addZIndexElement(elem, index) {
    const existing = this.zIndexElements.get(index);
    if (existing) {
      existing.push(elem);
    } else {
      this.zIndexElements.set(index, [elem]);
    }
  },

  addPositionedElement(elem) {
    this.positionedElements.push(elem);
  },

  addFloatingElement(elem) {
    this.floatingElements.push(elem);
  },

  addNonPositionedElement(elem) {
    this.nonPositionedElements.push(elem);
  },

  addChildren(parentNode) {
    for (const child of parentNode.children) {
      this.add(child, undefined, this.offset);
    }
  },

  addChildrenWithParent(parentElem) {
    for (const child of parentElem.raw.children) {
      this.add(child, parentElem, this.offset);
    }
  },

  // Get the elements in this context ordered back-to-front.
  flatten() {
    const rv = [];

    const pushElements = (elems) => {
      for (const elem of elems) {
        if (elem.context && elem.context != this) {
          rv.push(...elem.context.flatten());
        } else {
          rv.push(elem);
        }
      }
    };

    const pushZIndexElements = (filter) => {
      for (const z of zIndexes) {
        if (filter(z)) {
          pushElements(this.zIndexElements.get(z));
        }
      }
    };

    const zIndexes = [...this.zIndexElements.keys()];
    zIndexes.sort((a, b) => a - b);

    if (this.root) {
      pushElements([this.root]);
    }
    pushZIndexElements((z) => z < 0);
    pushElements(this.nonPositionedElements);
    pushElements(this.floatingElements);
    pushElements(this.positionedElements);
    pushZIndexElements((z) => z > 0);

    return rv;
  },
};

exports.StackingContext = StackingContext;
