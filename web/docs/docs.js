/*
 * docs.js — prism 開発者向け解説ドキュメントの補助スクリプト
 *
 * 役割は 2 つだけ: (1) 目次の現在地ハイライト、(2) 狭い画面での目次の開閉。
 * 外部依存なし。スクロール自体は CSS の scroll-behavior と <a href="#..."> に任せ、
 * ここでは IntersectionObserver で「今どの章を読んでいるか」を追うだけにする。
 */

'use strict';

(function () {
    const toc = document.getElementById('toc');
    const toggle = document.getElementById('tocToggle');
    const links = Array.from(document.querySelectorAll('.toc-list a'));
    const sections = links
        .map((a) => {
            const id = a.getAttribute('href').slice(1);
            return document.getElementById(id);
        })
        .filter(Boolean);

    function setActive(id) {
        for (const a of links) {
            const isActive = a.getAttribute('href') === '#' + id;
            a.classList.toggle('active', isActive);
            if (isActive) {
                a.setAttribute('aria-current', 'true');
            } else {
                a.removeAttribute('aria-current');
            }
        }
    }

    if ('IntersectionObserver' in window && sections.length > 0) {
        let current = sections[0].id;
        const observer = new IntersectionObserver(
            (entries) => {
                for (const entry of entries) {
                    if (entry.isIntersecting) {
                        current = entry.target.id;
                    }
                }
                setActive(current);
            },
            {
                rootMargin: '-15% 0px -70% 0px',
                threshold: 0,
            }
        );
        for (const s of sections) {
            observer.observe(s);
        }
        setActive(current);
    }

    if (toggle && toc) {
        toggle.addEventListener('click', () => {
            const open = toc.classList.toggle('open');
            toggle.setAttribute('aria-expanded', open ? 'true' : 'false');
        });
        for (const a of links) {
            a.addEventListener('click', () => {
                toc.classList.remove('open');
                toggle.setAttribute('aria-expanded', 'false');
            });
        }
    }
})();
