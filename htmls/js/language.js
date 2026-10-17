/* 같은 문서의 정적 번역 사이를 이동하며 링크의 쿼리와 앵커를 보존한다. */
/* Navigate between static translations of the same document, preserving query and anchor. */
(function (root) {
  'use strict';
  const languages = ['en', 'ko'];
  const storageKey = 'aevix.docs.language';

  function languageTarget(alternate, current) {
    const target = new URL(alternate, current);
    const source = new URL(current);
    if (target.origin !== source.origin) return null;
    target.search = source.search;
    target.hash = source.hash;
    return target.href;
  }

  function readPreference(storage) {
    try {
      const value = storage.getItem(storageKey);
      return languages.includes(value) ? value : null;
    } catch { return null; }
  }

  function writePreference(storage, language) {
    try { storage.setItem(storageKey, language); } catch { /* The selector still works without storage. */ }
  }

  function start(document, window) {
    const select = document.getElementById('docs-language');
    if (!select) return;
    const current = document.documentElement.lang === 'ko' ? 'ko' : 'en';
    const alternate = (language) => document.querySelector(`link[rel="alternate"][hreflang="${language}"]`)?.href;
    select.value = current;
    let storage;
    try { storage = window.localStorage; } catch { storage = null; }
    const preferred = readPreference(storage);
    // 명시적인 문서 URL은 존중하고, 사이트 입구에서만 마지막 선택을 복원한다.
    // Respect explicit document URLs and restore the last choice only at the site entrance.
    if (current === 'en' && preferred === 'ko' && window.location.pathname.endsWith('/')) {
      const href = alternate('ko');
      const target = href && languageTarget(href, window.location.href);
      if (target) { window.location.replace(target); return; }
    }
    select.addEventListener('change', () => {
      const language = select.value;
      if (!languages.includes(language)) return;
      writePreference(storage, language);
      if (language === current) return;
      const href = alternate(language);
      const target = href && languageTarget(href, window.location.href);
      if (target) window.location.assign(target);
      else select.value = current;
    });
  }

  const api = { languageTarget, readPreference, writePreference, start };
  if (typeof module === 'object' && module.exports) module.exports = api;
  else start(root.document, root);
})(typeof window === 'object' ? window : globalThis);
