/* 점진적인 문서 기능만 추가하며 코드는 이후 토큰화를 위해 이스케이프된 텍스트로 유지한다. */
/* Apply progressive enhancements only; keep code as escaped text for a future tokenizer. */
(() => {
  const typeMenu = document.querySelector('.type-menu');
  if (typeMenu) {
    document.addEventListener('click', (event) => {
      if (!typeMenu.contains(event.target)) typeMenu.open = false;
    });
    document.addEventListener('keydown', (event) => {
      if (event.key === 'Escape' && typeMenu.open) {
        typeMenu.open = false;
        typeMenu.querySelector('summary').focus();
      }
    });
  }
  const toc = document.querySelector('.toc');
  if (toc && window.matchMedia('(max-width: 780px)').matches) toc.open = false;

  document.querySelectorAll('pre').forEach((pre) => {
    const code = pre.querySelector('code');
    if (!code) return;
    pre.classList.add('code-block');
    if (pre.querySelector('.code-toolbar')) return;
    const languageClass = [...code.classList, ...pre.classList].find((name) => name.startsWith('language-'));
    const languageName = (code.dataset.language || pre.dataset.language || languageClass?.slice(9) || 'aevix').toLowerCase();
    const languageLabels = { aevix: 'Aevix · example syntax', sh: 'Shell', shell: 'Shell', bash: 'Bash', powershell: 'PowerShell', json: 'JSON', cpp: 'C++', text: 'Plain text' };
    code.classList.add(`language-${languageName}`);
    code.dataset.language = languageName;
    const toolbar = document.createElement('div');
    toolbar.className = 'code-toolbar';
    const language = document.createElement('span');
    language.textContent = languageLabels[languageName] || languageName;
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'copy-code';
    button.textContent = 'Copy';
    button.setAttribute('aria-label', 'Copy code example');
    button.setAttribute('aria-live', 'polite');
    let resetTimer;
    button.addEventListener('click', async () => {
      window.clearTimeout(resetTimer);
      button.disabled = true;
      try {
        await navigator.clipboard.writeText(code.textContent);
        button.textContent = 'Copied';
      } catch {
        // 로컬 파일이나 권한 거부로 복사에 실패하면 코드 자체를 선택한다.
        // Select the code itself when local-file restrictions or denied permissions prevent copying.
        const selection = window.getSelection();
        if (selection) {
          const range = document.createRange();
          range.selectNodeContents(code);
          selection.removeAllRanges();
          selection.addRange(range);
          button.textContent = 'Selected — copy manually';
        } else {
          button.textContent = 'Copy unavailable';
        }
      } finally {
        button.disabled = false;
      }
      resetTimer = window.setTimeout(() => { button.textContent = 'Copy'; }, 1800);
    });
    toolbar.append(language, button);
    pre.prepend(toolbar);
  });

  const updateTableFocus = () => {
    document.querySelectorAll('.table-scroll').forEach((region) => {
      if (region.scrollWidth > region.clientWidth + 1) region.tabIndex = 0;
      else region.removeAttribute('tabindex');
    });
  };
  updateTableFocus();
  window.addEventListener('resize', updateTableFocus, { passive: true });
  document.fonts?.ready.then(updateTableFocus);

  const search = document.getElementById('type-search');
  const languageRules = document.getElementById('Language-Rules');
  if (languageRules) {
    if (window.matchMedia('(max-width: 780px)').matches && location.hash !== '#Language-Rules') languageRules.open = false;
    window.addEventListener('hashchange', () => {
      if (location.hash === '#Language-Rules') languageRules.open = true;
    });
  }
  if (search) {
    const cards = [...document.querySelectorAll('[data-type-card]')];
    const status = document.getElementById('type-search-status');
    const empty = document.getElementById('type-search-empty');
    const updateSearch = () => {
      const query = search.value.trim().toLocaleLowerCase('en');
      let count = 0;
      cards.forEach((card) => {
        card.hidden = !card.dataset.typeCard.includes(query);
        if (!card.hidden) count += 1;
      });
      if (status) status.textContent = `${count} ${count === 1 ? 'family' : 'families'}`;
      if (empty) empty.hidden = count !== 0;
    };
    search.addEventListener('input', updateSearch);
    // 브라우저가 복원한 검색어도 첫 화면에서 즉시 적용한다.
    // Apply a browser-restored search query immediately on the first render.
    updateSearch();
  }

  if (!('IntersectionObserver' in window)) return;
  const links = [...document.querySelectorAll('.toc a[href^="#"]')];
  const sections = links.map((link) => document.getElementById(link.hash.slice(1))).filter(Boolean);
  const visible = new Set();
  const mark = (id) => links.forEach((link) => {
    if (link.hash.slice(1) === id) link.setAttribute('aria-current', 'location');
    else link.removeAttribute('aria-current');
  });
  const observer = new IntersectionObserver((entries) => {
    entries.forEach((entry) => entry.isIntersecting ? visible.add(entry.target) : visible.delete(entry.target));
    const current = sections.find((section) => visible.has(section));
    if (current) mark(current.id);
  }, { rootMargin: '-90px 0px -55% 0px', threshold: 0 });
  sections.forEach((section) => observer.observe(section));
  links.forEach((link) => link.addEventListener('click', () => mark(link.hash.slice(1))));
})();
