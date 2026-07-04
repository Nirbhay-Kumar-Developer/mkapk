// Mobile Menu Toggle
const hamburger = document.getElementById('hamburger');
const navLinks = document.querySelector('.nav-links');

if (hamburger) {
    hamburger.addEventListener('click', () => {
        hamburger.classList.toggle('active');
        navLinks.classList.toggle('active');
    });
}

// Close mobile menu when clicking on a link
if (navLinks) {
    const links = navLinks.querySelectorAll('a');
    links.forEach(link => {
        link.addEventListener('click', () => {
            hamburger.classList.remove('active');
            navLinks.classList.remove('active');
        });
    });
}

// FAQ Accordion
const faqItems = document.querySelectorAll('.faq-item');
faqItems.forEach(item => {
    const question = item.querySelector('.faq-question');
    if (question) {
        question.addEventListener('click', () => {
            // Close other items
            faqItems.forEach(other => {
                if (other !== item) {
                    other.classList.remove('active');
                }
            });
            // Toggle current item
            item.classList.toggle('active');
        });
    }
});

// Back to Top Button
const backToTop = document.querySelector('.back-to-top');
if (backToTop) {
    window.addEventListener('scroll', () => {
        if (window.scrollY > 300) {
            backToTop.classList.add('show');
        } else {
            backToTop.classList.remove('show');
        }
    });

    backToTop.addEventListener('click', () => {
        window.scrollTo({
            top: 0,
            behavior: 'smooth'
        });
    });
}

// Smooth Scroll for anchor links
document.querySelectorAll('a[href^="#"]').forEach(anchor => {
    anchor.addEventListener('click', function(e) {
        const href = this.getAttribute('href');
        if (href !== '#') {
            e.preventDefault();
            const target = document.querySelector(href);
            if (target) {
                target.scrollIntoView({
                    behavior: 'smooth'
                });
            }
        }
    });
});

// Add active class to navigation based on scroll position
function updateActiveNav() {
    const navLinks = document.querySelectorAll('.nav-links a');
    const sections = document.querySelectorAll('section');

    let current = '';
    sections.forEach(section => {
        const sectionTop = section.offsetTop;
        if (scrollY >= sectionTop - 200) {
            current = section.getAttribute('id');
        }
    });

    navLinks.forEach(link => {
        link.classList.remove('active');
        if (link.getAttribute('href') === '#' + current) {
            link.classList.add('active');
        }
    });
}

window.addEventListener('scroll', updateActiveNav);

// Copy to clipboard functionality
function setupCopyButtons() {
    const codeBlocks = document.querySelectorAll('.code-block pre, .command-box');
    codeBlocks.forEach(block => {
        const button = document.createElement('button');
        button.className = 'copy-btn';
        button.innerHTML = '<i class="fas fa-copy"></i> Copy';
        button.style.cssText = `
            position: absolute;
            top: 10px;
            right: 10px;
            padding: 0.5rem 1rem;
            background: var(--primary-color);
            color: #fff;
            border: none;
            border-radius: 4px;
            cursor: pointer;
            font-size: 0.9rem;
            font-weight: 600;
        `;

        block.parentElement.style.position = 'relative';
        block.parentElement.appendChild(button);

        button.addEventListener('click', () => {
            const text = block.textContent;
            navigator.clipboard.writeText(text).then(() => {
                const originalText = button.innerHTML;
                button.innerHTML = '<i class="fas fa-check"></i> Copied!';
                setTimeout(() => {
                    button.innerHTML = originalText;
                }, 2000);
            });
        });
    });
}

setupCopyButtons();

// Initialize tooltips and popovers if needed
function initializeTooltips() {
    const tooltips = document.querySelectorAll('[data-tooltip]');
    tooltips.forEach(element => {
        element.addEventListener('mouseenter', function() {
            const text = this.getAttribute('data-tooltip');
            const tooltip = document.createElement('div');
            tooltip.className = 'tooltip';
            tooltip.textContent = text;
            tooltip.style.cssText = `
                position: absolute;
                background: #333;
                color: #fff;
                padding: 0.5rem 1rem;
                border-radius: 4px;
                font-size: 0.85rem;
                z-index: 1000;
                white-space: nowrap;
            `;
            document.body.appendChild(tooltip);

            const rect = this.getBoundingClientRect();
            tooltip.style.left = rect.left + 'px';
            tooltip.style.top = (rect.top - tooltip.offsetHeight - 10) + 'px';

            this.addEventListener('mouseleave', () => tooltip.remove());
        });
    });
}

initializeTooltips();

// Search functionality (basic)
function setupSearch() {
    const searchInput = document.querySelector('.search-input');
    if (searchInput) {
        searchInput.addEventListener('input', (e) => {
            const query = e.target.value.toLowerCase();
            if (query.length > 2) {
                performSearch(query);
            }
        });
    }
}

function performSearch(query) {
    // This would typically search through content or use an external API
    console.log('Searching for:', query);
}

setupSearch();

// Initialize on DOM load
document.addEventListener('DOMContentLoaded', () => {
    console.log('mkapk Documentation Loaded');
    updateActiveNav();
});

// Performance monitoring
if (window.performance && window.performance.timing) {
    window.addEventListener('load', () => {
        const perfData = window.performance.timing;
        const pageLoadTime = perfData.loadEventEnd - perfData.navigationStart;
        console.log('Page load time:', pageLoadTime + 'ms');
    });
}