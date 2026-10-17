========================

Created By : miruku_sora2026

File name : README.txt

Description :
    영어 원문과 자체 한국어 문서의 갱신 방법을 설명한다.
    Explain maintenance of the English source and native Korean documentation.

========================

Documentation languages

English sources live directly under htmls/. Korean pages mirror the same
paths under htmls/ko/. Both versions are ordinary static HTML and work
without a translation service. JavaScript adds preference storage and a
language selector; it does not translate page contents at runtime.

ko.json maps normalized English text to Korean. Edit translations there,
then regenerate Korean HTML instead of editing generated pages directly:

    python3 tools/docs/build_locales

After adding or updating English pages, export the current source strings:

    python3 tools/docs/build_locales --prepare --export /tmp/aevix-i18n-source.json

Add translations for new strings to ko.json and run the generator again.
Missing translations stop the build before any Korean page is written.
After deleting or renaming an English page, explicitly remove or archive
its old Korean counterpart. The generator reports these orphaned pages
and stops rather than silently deleting files.
Unchanged English text reuses its existing translation. Review translated
sentences in context, especially fragments split by inline code or links.
An implemented feature, a selected design rule, and a future proposal are
different statuses; keep these distinctions in both languages.

The generator preserves code, commands, identifiers inside code tags,
element IDs, and URL fragments. It rewrites local document links to the
matching Korean page and keeps CSS, scripts, fonts, and images shared.
Translation data is HTML-escaped before insertion.

Check navigation and the generated tree:

    node --test tests/docs/language_tests.cjs
    python3 tests/docs/locale_tests
    node --check htmls/js/language.js
    node --check htmls/js/docs.js

Also inspect the home page and a deep reference page in a browser, at
desktop and narrow mobile widths. Check selection in both directions,
query strings, anchors, Korean type search, and code copying.

The initial catalog combines an offline machine translation draft with
reviewed terminology and manual translations of the home page, release
profiles, roadmaps, and core language rules. It is not a claim that every
sentence has received a separate human review. Credits and the pinned
model revision are recorded in NOTICE.txt and ko.json metadata.

tools/docs/translate_catalog is an optional draft-generation tool. It is
not required to build or serve documentation. Its model and conversion
dependencies belong in an isolated local environment; model weights and
translation caches are not distributed with this project.
