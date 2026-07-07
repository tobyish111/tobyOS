# Browser stage 13A: CSS custom properties + var()

Branch `browser-css-vars` (first item of the stage-13 Chrome-parity
push, off `browser-reach` so it stacks on all of stage 12). Custom
properties (`--name`) and `var(--name, fallback)` are used on
essentially every modern site for theming and sizing; without them,
declarations that reference a variable silently drop and styling
collapses even when the layout primitives are present. This is the
single highest-leverage CSS item.

## What landed (programs/user_gui_browser/main.c)

- **Parsing**: a declaration whose property name starts with `--` is
  tagged `CP_VAR` and keeps its name in the CSS pool (normal
  declarations still discard the name as transient). `struct cdecl`
  gained `noff`/`nlen` for the custom-property name.
- **Scoped variable stack**: custom properties cascade and inherit, so
  they are collected into a stack during `style_node`, an entry per
  definition with name+value **copied into a stable pool** (they must
  survive the per-node `csspool` rollback and outlive the node so its
  whole subtree can resolve them). Each node records the stack mark on
  entry, pushes its own custom properties in cascade order (matched
  non-important, inline non-important, matched important, inline
  important — lowest priority first so the last-pushed wins on lookup),
  styles itself and recurses into its subtree, then pops back to the
  mark. This gives correct **scoping** (a variable set on one subtree
  does not leak to siblings) and **inheritance** (descendants see
  ancestor variables).
- **var() substitution**: `st_apply` checks each value for `var(` and,
  if present, expands `var(--name)` / `var(--name, fallback)` against
  the current variable stack into a scratch buffer before the
  property-specific parser runs. Substitution recurses (depth-limited)
  so a variable's value may itself use `var()`. Nearest-scope /
  highest-priority definition wins; an undefined variable with no
  fallback expands to empty.
- **`:root` selector**: added to the selector parser (maps to the
  `html` element) — the canonical place custom properties are defined.

Pages with no custom properties take byte-identical code paths (the
`var(` scan returns immediately, the collection loops find nothing), so
the change is purely additive.

## Verified in QEMU (screenshot in the session scratchpad)
The `/vars` page, all six cases correct:
1. A card whose border, background, and padding all come from `:root`
   variables.
2. `var(--accent)` — red bold text.
3. `var(--missing, #188038)` — green via the **fallback** (variable
   undefined).
4. `--local: var(--accent); color: var(--local)` — red via a variable
   that **references another variable**.
5. A box that redefines `--brand` locally: its text and border go
   orange...
6. ...while the heading above it stays blue — the **scope does not leak
   up** (the hard correctness case, proving the push/pop stack).

Regressions: the CSS torture page and tables page are unaffected (no
custom properties → identical cascade).

## v1 limits / next
- `calc()` (often paired with variables, e.g. `calc(var(--gap) * 2)`)
  is the next branch; a value that is a bare `calc(...)` still fails to
  parse today (unchanged behavior).
- Custom-property names are lowercased (CSS treats them case-sensitive);
  since both definition and `var()` use are lowercased consistently,
  they still match — an edge only if a page relies on case-distinct
  variable names, which is vanishingly rare.
- No `@property` typed custom properties, no computed-value-time
  cycles detection beyond the depth limit.
