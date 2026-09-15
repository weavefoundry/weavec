// Starlight tables scroll horizontally on narrow screens. Preserve their native
// table semantics while making that scroll area reachable from the keyboard.
export function accessibleTables() {
  return {
    name: 'keyboard-accessible-tables',
    element: {
      filter: ['table'],
      visit(node, context) {
        context.setProperty(node, 'tabIndex', 0);
      },
    },
  };
}
