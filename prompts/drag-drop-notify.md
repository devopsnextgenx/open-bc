# OpenBC: File Change Detection, Smart Drag-and-Drop, and Tab-Aware Comparison

## Objective

Implement a cohesive file lifecycle and drag-and-drop experience across the OpenBC application, following the existing architecture and conventions in:

- `openbc-ui-qt`
- `openbc-vfs`
- `openbc-core`
- `openbc-engine`
- Existing comparison modules and file-type-specific implementations, including image, text, CSV, registry, hex, media, 3D, and folder comparison.

This task consists of three major subtasks:

1. External file-change detection and user-controlled reload.
2. Drag-and-drop integration for files and folders from desktop file managers.
3. Tab-aware drop routing, file-type detection, and intelligent comparison-tab creation or conversion.

Implement these features as a unified solution, not as unrelated UI patches.

## 1. Mandatory repository reconnaissance

Before changing code, inspect the actual repository and establish how the existing application works.

1. Read `README.md`, `ARCHITECTURE.md`, relevant crate manifests, and existing design documentation.
2. Trace the current file-opening workflow from `openbc-ui-qt` through the engine and VFS.
3. Identify:
   - Existing tab models and tab lifecycle.
   - How tabs represent comparison types.
   - How the home tab opens comparison tabs.
   - How files and folders are currently selected, opened, and compared.
   - How VFS paths, local paths, remote resources, and file metadata are represented.
   - Existing asynchronous event delivery and tab-specific actors.
   - Existing file-type detection and comparison dispatch.
   - Existing unsaved-state, dirty-state, and reload behavior.
   - Existing Qt drag-and-drop event handlers and MIME-data handling.
4. Find reusable abstractions before introducing new ones. Extend the existing architecture wherever practical.
5. Identify crate dependency constraints and keep the dependency graph valid.

Do not assume that proposed type names or APIs already exist. Inspect the source and adapt the implementation to the real interfaces.

Produce a brief implementation plan mapping each feature to the appropriate modules, then implement the changes. Do not stop after producing the plan.

## 2. Subtask A: External file-change detection and reload

### A1. Detect changes to loaded resources

Implement reusable file-change observation in `openbc-vfs` or the appropriate existing VFS abstraction.

Support:

- Local file modification.
- File deletion, replacement, and atomic-save workflows.
- Local directory changes where relevant to folder comparison.
- Remote resource changes where the underlying adapter supports observation or version checks.
- Remote polling when push notifications are unavailable.
- Resource unavailability and permission errors.
- Multiple tabs observing the same resource without creating unnecessary independent watchers.

Use native filesystem notifications where appropriate, with polling or another fallback when necessary. For remote resources, prefer authoritative revision identifiers, ETags, or equivalent metadata when available.

Treat notifications as hints to recheck resource state. Do not assume that a timestamp or a watcher event alone proves that the file contents changed.

Avoid unnecessarily reading entire large files just to detect changes. Use the strongest practical version information exposed by each adapter.

### A2. Track the version loaded by each comparison

In the appropriate engine or document-state abstraction, track the version of each resource used to produce the currently loaded comparison.

When a resource changes:

1. Determine which tabs depend on that resource.
2. Compare the current resource version with the version used by each affected tab.
3. Mark affected tabs as stale only when a change is established.
4. Emit a typed, tab-aware event to the UI.
5. Keep the loaded content unchanged until an explicit reload or another existing application operation replaces it.

Support comparisons involving multiple inputs, including image pairs, text pairs, CSV pairs, binary files, registry files, media, 3D resources, and folder comparisons.

For folder comparison, distinguish changes to directory contents from changes to individual files where the existing comparison model permits it.

### A3. Notify the user in the affected tab

In `openbc-ui-qt`, provide a non-intrusive notification associated with the relevant tab.

Example:

"File changed externally. The version currently displayed may be outdated."

Offer two explicit actions:

- **Reload Latest:** Read the current resource and refresh the appropriate comparison.
- **Keep Current:** Continue using the already-loaded content without saving or reloading it.

For two-input comparisons, clearly identify which input or inputs changed.

Expected behavior:

- Reload updates the affected input and recomputes the comparison as required.
- Keep Current retains the current loaded content.
- Dismissing a notification does not silently reload the file.
- A stale indicator may remain visible after the notification is dismissed.
- Repeated watcher events for the same version should not produce notification spam.
- A genuinely newer version can trigger a new notification.
- Closing a tab unregisters its observations when no longer needed.
- Multiple tabs using the same resource receive appropriate notifications independently.

Handle file deletion or temporary remote unavailability without crashing or silently replacing loaded content.

### A4. Preserve user data and prevent reload races

Do not implement reload as an unconditional file reopen.

- Preserve any existing dirty-state or unsaved-change protections.
- If the application supports editing, never discard unsaved edits without the existing confirmation policy or an explicit user decision.
- If a file changes again during reload, do not incorrectly mark an older read as the latest version.
- Perform filesystem operations asynchronously where appropriate.
- Deliver UI updates on the appropriate Qt thread.
- Cancel or ignore stale asynchronous results when a tab closes or its inputs change.
- Avoid deadlocks, unbounded polling, and watcher leaks.

Keep file observation, stale-state tracking, reload coordination, and presentation separate.

## 3. Subtask B: Drag-and-drop from desktop file managers

Implement drag-and-drop support for files and directories selected in external desktop file managers, including Nautilus, Dolphin, and other compatible file managers.

Use Qt's native drag-and-drop and MIME-data mechanisms. Support standard local-file URL payloads and any existing remote URL or virtual-file support where practical.

### B1. Drop two selected folders into Folder Compare

When a user selects two folders in Nautilus, Dolphin, or another supported file manager and drops them onto a Folder Compare tab:

1. Parse the dropped MIME data.
2. Normalize and validate the dropped paths.
3. Recognize that the operation supplies two comparison inputs.
4. Populate the left and right folder inputs.
5. Start the existing folder comparison workflow.

Expected behavior:

- Two valid directories populate the two sides in a deterministic order.
- One directory does not accidentally populate both sides.
- More than two dropped items are handled according to an explicit, documented policy rather than silently choosing arbitrary paths.
- Invalid, inaccessible, or unsupported items produce an appropriate error or feedback message.
- Directory comparison uses the existing engine and VFS implementation.
- No duplicate comparison tab is created unnecessarily when the drop is handled by an existing Folder Compare tab.

The feature must work when the user drops the folders directly onto the folder comparison content area or its designated input area, consistent with the existing UI design.

### B2. Drop files or folders onto the Home tab

The Home tab should act as an entry point for creating the appropriate comparison.

When files or folders are dropped onto Home:

1. Inspect all dropped items.
2. Resolve their types using the existing file-type detection mechanisms.
3. Determine the appropriate comparison type.
4. Open or initialize the corresponding comparison tab.
5. Populate its inputs and initiate the existing workflow.

Examples:

- Two image files → Image Compare.
- Two text files → Text Compare.
- Two CSV files → CSV Compare.
- Two registry files → Registry Compare.
- Two binary files → Hex Compare or the existing binary comparison mode.
- Two media files → Media Compare.
- Two 3D resources → 3D Compare.
- Two folders → Folder Compare.
- A single supported file → the appropriate single-file workflow, preview, or input state supported by the application.

These are routing examples, not instructions to invent comparison modes. Inspect the actual supported types and use existing dispatch rules.

When multiple dropped items have incompatible types, do not silently force them into an inappropriate comparison. Use existing mixed-type comparison support if available; otherwise provide a predictable, user-friendly fallback.

If several independent comparisons can be inferred from the dropped items, group them only when the grouping rules are unambiguous. Otherwise, use an explicit fallback that preserves every dropped item and avoids unexpected tab proliferation.

### B3. Correct Qt event handling

Ensure that:

- `dragEnterEvent` accepts supported payloads.
- `dragMoveEvent` provides appropriate feedback when the target accepts the operation.
- `dropEvent` handles the final payload and reports the appropriate result.
- MIME parsing and filesystem validation are robust.
- Unsupported payloads are rejected without corrupting tab state.
- Drag-and-drop works for both file and folder URLs.
- UI-thread requirements are respected.
- Existing selection, keyboard, context-menu, and file-opening workflows remain functional.

Do not implement separate, conflicting drag-and-drop logic in every widget. Introduce a reusable routing or parsing abstraction in the layer that best fits the existing architecture.

## 4. Subtask C: Tab-aware drops and intelligent comparison routing

This is a central requirement: **the destination tab, the dropped resources, and their detected types must all participate in deciding what happens.**

A drop should not be routed solely by file extension, nor should it always overwrite the currently selected tab.

### C1. Define a central drop-routing policy

Implement a reusable drop-routing decision that considers:

- The destination tab and its current comparison type.
- The number and types of dropped resources.
- The current left/right inputs, if any.
- Whether the existing tab is empty, initialized, or actively comparing.
- Whether the dropped resources match the tab's supported type.
- Whether a conversion to another comparison type is supported.
- Whether the operation could discard unsaved or otherwise valuable state.

Use the application's existing type registry, comparison factory, or dispatch mechanism wherever possible.

The UI may collect drag-and-drop input, but it should not duplicate the engine's file-type dispatch rules.

### C2. Drop compatible files onto an existing comparison tab

When a compatible file or folder is dropped onto a comparison tab:

- Reuse the existing tab.
- Populate or replace the appropriate comparison input according to the existing application's input-selection behavior.
- For two dropped resources, assign the two sides consistently.
- If the tab already contains inputs, follow a documented replacement policy.
- Do not create another tab unless the user-facing workflow or existing architecture requires it.

Do not unexpectedly replace both inputs when the user drops a single file. Preserve the unaffected input where that matches the application's existing interaction model.

### C3. Handle incompatible types intelligently

Example: a user has a Text Compare tab selected and drops two image files onto it.

The application must not attempt to load the images as text or silently reject a valid drop.

Apply the following decision policy:

1. If the destination tab is empty or has no meaningful comparison state, convert or initialize it as the correct comparison type if the existing tab architecture supports safe conversion.
2. If the tab is active and contains meaningful state, follow the application's existing safe tab-transition behavior. If conversion can preserve the tab identity and safely replace its comparison state, reuse the tab; otherwise, open a new tab of the appropriate type.
3. If the dropped items cannot be compared together, preserve the items and provide a clear fallback rather than silently losing input.
4. Never discard unsaved work or destroy a comparison in progress without applying the application's existing confirmation or preservation rules.

For example:

- Text Compare + two images → Image Compare or a new Image Compare tab.
- Image Compare + two text files → Text Compare or a new Text Compare tab.
- Hex Compare + two CSV files → CSV Compare or a new CSV Compare tab.
- Folder Compare + two files → the appropriate file comparison workflow.
- Home + two folders → Folder Compare.
- Home + two compatible media files → Media Compare.
- An existing tab with an incompatible type and meaningful state → preserve the original state and open a suitable destination when safe in-place conversion is not supported.

Use the real comparison types supported by the repository, including 3D, media, text, CSV, registry, hex, image, and folder comparison where implemented.

### C4. Preserve tab identity where appropriate

When converting a tab in place:

- Update its comparison model, controller, or actor according to the existing lifecycle.
- Reset incompatible input models, caches, and UI state.
- Cancel obsolete asynchronous operations.
- Disconnect obsolete event subscriptions.
- Attach the correct comparison events and resource observers.
- Update tab title, icon, type indicators, and other type-specific UI.
- Prevent old results from appearing in the newly initialized tab.
- Ensure that the tab does not reference destroyed models or actors.

If the existing architecture does not support safe conversion, prefer creating the correct new tab over introducing a fragile partial conversion.

### C5. Define deterministic behavior for all drop combinations

Document the routing behavior for:

- One file.
- Two compatible files.
- Two incompatible files.
- More than two files.
- One folder.
- Two folders.
- Mixed files and folders.
- Supported and unsupported file types.
- Drops onto Home.
- Drops onto compatible comparison tabs.
- Drops onto incompatible comparison tabs.
- Drops onto an empty tab.
- Drops onto a tab with an active comparison.
- Drops onto a tab with unsaved or otherwise valuable state.

The routing policy should be deterministic, testable, and reusable by Home and all comparison tabs.

## 5. Architectural responsibilities

Respect the current repository architecture rather than moving all behavior into Qt.

### `openbc-vfs`

Own the filesystem-specific observation and resource-access mechanisms:

- File and directory version checks.
- Local watcher adapters and appropriate fallbacks.
- Remote revision checks or polling.
- Reusable resource metadata and observation abstractions.
- Adapter-specific limitations and error handling.

Do not put user-facing notifications or comparison-tab policy here.

### `openbc-core`

Own shared domain types where appropriate:

- Resource version or fingerprint types.
- Typed file-change events.
- Stale-resource state, if it belongs in the domain model.
- Shared drop-routing inputs or outcomes only if they are genuinely cross-layer domain concepts.

Avoid introducing filesystem, Qt, or UI dependencies into core.

### `openbc-engine`

Own orchestration and comparison lifecycle:

- Tracking versions of loaded resources.
- Detecting stale loaded inputs.
- Managing resource-observation subscriptions where architecturally appropriate.
- Emitting tab-aware events.
- Coordinating reload and recomputation.
- Resolving comparison types using existing dispatch mechanisms.
- Initializing or safely replacing comparison state.
- Preventing races between loading, reloading, and tab changes.

Do not create a second, competing comparison-routing system if one already exists.

### `openbc-ui-qt`

Own presentation and input interaction:

- Native drag-and-drop event handling.
- Parsing or delegating MIME payloads.
- Home-tab drop handling.
- Per-tab file-change notifications.
- Reload and Keep Current actions.
- User confirmation where required.
- Tab-level feedback, status, and safe conversion presentation.

Do not independently watch the filesystem or duplicate remote change-detection logic in the UI.

### Comparison-specific modules

Reuse the existing image, text, CSV, registry, hex, media, 3D, and folder comparison implementations. Each comparison type should continue to own its existing loading, validation, and comparison logic.

Do not create duplicate comparison implementations just to support drag-and-drop.

## 6. Testing requirements

Add automated tests using the repository's existing testing conventions.

### File-change detection

- Local modification is detected.
- Atomic file replacement is handled.
- Deleted files are handled safely.
- Unchanged files do not repeatedly trigger stale notifications.
- Remote revisions are detected when supported.
- Polling and observation failures are handled safely.
- Multiple tabs observing the same file are updated independently.
- Closed tabs no longer receive relevant events.
- Keep Current preserves the loaded content.
- Reload uses the latest available content.
- Changes occurring during reload do not produce incorrect freshness state.

### Drag-and-drop

- Two local folders dropped on Folder Compare populate the correct sides.
- One folder does not initialize both sides.
- Two files dropped on Home select the appropriate comparison.
- Two folders dropped on Home open Folder Compare.
- Unsupported or mixed types produce predictable results.
- Invalid MIME payloads are rejected safely.
- Dropped paths with spaces and non-ASCII characters are handled correctly.
- Remote URLs are handled according to existing VFS capabilities.

### Tab-aware routing

- Compatible drops reuse the destination tab.
- Incompatible drops route to the appropriate comparison type.
- An empty tab can be initialized or converted safely.
- An active incompatible tab is preserved when necessary.
- Tab identity is preserved only when conversion is safe and supported.
- Obsolete asynchronous results cannot overwrite newly initialized state.
- Correct events, observers, and models are attached after conversion.
- Existing keyboard, menu, and ordinary file-opening behavior still works.

Add unit tests for the routing decision independently of Qt widgets, and integration tests for the end-to-end workflows where the existing test infrastructure supports them.

## 7. Implementation constraints

- Follow existing naming, formatting, error-handling, and async conventions.
- Minimize unnecessary dependencies.
- Do not introduce a new framework unless the existing architecture cannot reasonably support the feature.
- Avoid blocking the Qt UI thread.
- Do not rely exclusively on file extensions for type detection.
- Preserve current comparison behavior and user data.
- Avoid duplicate watchers, duplicate event streams, tab leaks, and orphaned background tasks.
- Avoid breaking public APIs unless the change is necessary and justified.
- Do not claim remote change detection is supported where the underlying adapter cannot provide it.
- Ensure the project builds and existing tests continue to pass.

## 8. Required deliverables

Implement the features in the repository and provide:

1. A concise architecture summary explaining which modules own each responsibility.
2. A list of modified files and the purpose of each change.
3. The implemented VFS observation mechanism and its local/remote limitations.
4. The file-change event flow from VFS through the engine to Qt.
5. The drag-and-drop routing policy and how comparison types are selected.
6. An explanation of when tabs are reused, converted, or newly created.
7. Tests added and their results.
8. Build and test commands executed, including any failures or environmental limitations.
9. Any remaining limitations or follow-up work.

## Definition of done

The feature is complete when:

- A change to a loaded local or supported remote resource is detected and the affected tab is notified.
- The user can explicitly reload the latest resource or continue with the existing loaded version without saving or silently reloading it.
- Two folders dropped from a desktop file manager into Folder Compare populate the two comparison inputs.
- Dropping files or folders on Home opens the correct supported comparison workflow.
- Dropping resources onto an existing tab follows a consistent, type-aware routing policy.
- An incompatible drop never attempts to load a resource using the wrong comparison implementation.
- Tabs are safely reused, converted, or newly created according to the documented policy.
- Existing state is preserved whenever a safe transition cannot be guaranteed.
- The relevant tests pass, the project builds where the environment permits, and the final implementation follows the repository's actual architecture.

Start by inspecting the repository and tracing the existing flows. Then implement the subtasks in dependency order, validating each layer before integrating the next. Do not replace existing abstractions with speculative new ones or leave the task at the planning stage.