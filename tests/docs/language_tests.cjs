// ========================
//
// Created By : miruku_sora2026
//
// File name : language_tests.cjs
//
// Description :
//     언어 선택의 경로 보존, 저장 실패, 재방문 동작을 검사한다.
//     Check language navigation, unavailable storage, and returning visits.
//
// ========================

const test = require('node:test');
const assert = require('node:assert/strict');
const { languageTarget, readPreference, start } = require('../../htmls/js/language.js');

function page({ current = 'en', href = 'https://example.test/Aevix-lang/index.html', preference = null, blocked = false } = {}) {
  const listeners = {};
  const navigations = [];
  const saved = [];
  const select = { value: current, addEventListener: (name, fn) => { listeners[name] = fn; } };
  const document = {
    documentElement: { lang: current },
    getElementById: () => select,
    querySelector: (selector) => ({ href: `https://example.test/Aevix-lang/${selector.includes('"ko"') ? 'ko/' : ''}index.html` }),
  };
  const url = new URL(href);
  const window = {
    location: {
      href: url.href, pathname: url.pathname,
      assign: (target) => navigations.push(['assign', target]),
      replace: (target) => navigations.push(['replace', target]),
    },
    get localStorage() {
      if (blocked) throw new Error('Storage denied');
      return { getItem: () => preference, setItem: (key, value) => saved.push([key, value]) };
    },
  };
  start(document, window);
  return { select, listeners, navigations, saved };
}

test('a deep document switches language without losing deployment prefix, query, or anchor', () => {
  assert.equal(languageTarget('../../ko/grammar/modules/Modules_and_Paths.html',
    'https://example.test/Aevix-lang/grammar/modules/Modules_and_Paths.html?q=path#Module-Identity'),
  'https://example.test/Aevix-lang/ko/grammar/modules/Modules_and_Paths.html?q=path#Module-Identity');
  assert.equal(languageTarget('../../../grammar/modules/Modules_and_Paths.html',
    'https://example.test/Aevix-lang/ko/grammar/modules/Modules_and_Paths.html#Imports'),
  'https://example.test/Aevix-lang/grammar/modules/Modules_and_Paths.html#Imports');
  assert.equal(languageTarget('https://outside.test/ko/index.html', 'https://example.test/index.html'), null);
});

test('the native selector records a preference and navigates to its alternate', () => {
  const state = page({ href: 'https://example.test/Aevix-lang/index.html?source=docs#Quick-Start' });
  state.select.value = 'ko';
  state.listeners.change();
  assert.deepEqual(state.saved, [['aevix.docs.language', 'ko']]);
  assert.deepEqual(state.navigations, [['assign', 'https://example.test/Aevix-lang/ko/index.html?source=docs#Quick-Start']]);
});

test('blocked browser storage does not prevent switching', () => {
  const state = page({ blocked: true });
  state.select.value = 'ko';
  assert.doesNotThrow(() => state.listeners.change());
  assert.equal(state.navigations.length, 1);
  assert.equal(readPreference({ getItem: () => 'unsupported' }), null);
});

test('returning to the site entrance restores Korean without adding a redirect history entry', () => {
  const state = page({ href: 'https://example.test/Aevix-lang/?q=first#Quick-Start', preference: 'ko' });
  assert.deepEqual(state.navigations, [['replace', 'https://example.test/Aevix-lang/ko/index.html?q=first#Quick-Start']]);
});

test('explicit document URLs and an unchanged selector never create redirect loops', () => {
  const state = page({ preference: 'ko' });
  assert.deepEqual(state.navigations, []);
  state.listeners.change();
  assert.deepEqual(state.navigations, []);
  const korean = page({ current: 'ko', preference: 'en', href: 'https://example.test/Aevix-lang/ko/' });
  assert.deepEqual(korean.navigations, []);
  korean.select.value = 'en';
  korean.listeners.change();
  assert.deepEqual(korean.navigations, [['assign', 'https://example.test/Aevix-lang/index.html']]);
});
