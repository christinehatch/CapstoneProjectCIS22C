// test_frontend.mjs
import puppeteer from "puppeteer";

const BASE_URL = "http://localhost:8080";

/**
 * Small helper to log test titles and pass/fail status.
 */
async function runTest(name, fn, page) {
  process.stdout.write(`\n[TEST] ${name} ... `);
  try {
    await fn(page);
    console.log("✅ PASS");
  } catch (err) {
    console.log("❌ FAIL");
    console.error("   ", err.message || err);
    throw err;
  }
}

/**
 * 1) Route Finder basic: loads and shows one-hop results.
 */
async function testRouteFinderLoads(page) {
  await page.goto(`${BASE_URL}/`, { waitUntil: "networkidle0" });

  // Fill in SFO -> JFK
  await page.type("#src", "SFO");
  await page.type("#dst", "JFK");

  // Submit the "Search One-Hop Routes" form
  await Promise.all([
    page.click('form#route-form button[type="submit"]'),
    page.waitForResponse((resp) =>
      resp.url().includes("/onehop") && resp.status() === 200
    ),
  ]);

  // Wait for any .route-card (one-hop result cards) to appear
  await page.waitForSelector(".route-card", { timeout: 5000 });

  // Basic sanity check: at least one result card
  const count = await page.$$eval(".route-card", (els) => els.length);
  if (count === 0) {
    throw new Error("No one-hop route cards found after search.");
  }
}

/**
 * 2) Route Finder: JSON tab works (view-mode toggle).
 */
async function testRouteFinderJsonTab(page) {
  // Assume we already did a search in previous test, but be safe:
  await page.goto(`${BASE_URL}/`, { waitUntil: "networkidle0" });
  await page.type("#src", "SFO");
  await page.type("#dst", "JFK");

  await Promise.all([
    page.click('form#route-form button[type="submit"]'),
    page.waitForResponse((resp) =>
      resp.url().includes("/onehop") && resp.status() === 200
    ),
  ]);

  await page.waitForSelector(".route-card", { timeout: 5000 });

  // Click JSON tab
  await page.click('#viewModeTabs .view-tab[data-mode="json"]');

  // Make sure a <pre> with JSON shows up
  await page.waitForSelector("#resultsList pre", { timeout: 5000 });
  const jsonText = await page.$eval("#resultsList pre", (pre) => pre.textContent || "");
  if (!jsonText.includes('"one_hop_routes"') && !jsonText.includes('"src"')) {
    throw new Error("JSON view does not contain expected keys.");
  }

  // Switch back to Cards and ensure cards re-appear
  await page.click('#viewModeTabs .view-tab[data-mode="cards"]');
  await page.waitForSelector(".route-card", { timeout: 5000 });
}

/**
 * 3) Airline UI: search AA and verify airports show.
 */
async function testAirlineUiAA(page) {
  await page.goto(`${BASE_URL}/airline-ui`, { waitUntil: "networkidle0" });

  // Enter "AA" and submit
  await page.type("#airline", "AA");
  await Promise.all([
    page.click("form#airline-form button[type=submit]"),
    page.waitForResponse((resp) =>
      resp.url().includes("/airline/routes") && resp.status() === 200
    ),
  ]);

  // Wait for summary airline card
  await page.waitForSelector(".route-card", { timeout: 5000 });

  // Ensure at least one airport card appears after the airline summary
  const cards = await page.$$eval(".route-card", (els) => els.length);
  if (cards < 2) {
    throw new Error("Expected at least one airport card for AA.");
  }

  // Check that the summary card text contains "AA"
  const summaryText = await page.$eval(".route-card", (el) => el.textContent || "");
  if (!summaryText.includes("AA")) {
    throw new Error("Airline summary card does not mention AA.");
  }
}

/**
 * 4) Airport UI: search LGA and verify airlines show.
 */
async function testAirportUiLGA(page) {
  await page.goto(`${BASE_URL}/airport-ui`, { waitUntil: "networkidle0" });

  await page.type("#airport", "LGA");

  await Promise.all([
    page.click("form#airport-form button[type=submit]"),
    page.waitForResponse((resp) =>
      resp.url().includes("/airport/routes") && resp.status() === 200
    ),
  ]);

  // Wait for airport summary card
  await page.waitForSelector(".card", { timeout: 5000 });

  // Check at least one airline row-card
  const rows = await page.$$eval(".route-card", (els) => els.length);
  if (rows === 0) {
    throw new Error("Expected at least one airline route-card for LGA.");
  }

  const summaryText = await page.$eval(".card", (el) => el.textContent || "");
  if (!summaryText.includes("LGA")) {
    throw new Error("Airport summary card does not mention LGA.");
  }
}

/**
 * 5) NEW: Route Finder airline filter pills on one-hop results.
 */
async function testRouteFinderAirlineFilterPills(page) {
  await page.goto(`${BASE_URL}/`, { waitUntil: "networkidle0" });

  // Run the same SFO -> JFK one-hop search
  await page.type("#src", "SFO");
  await page.type("#dst", "JFK");

  await Promise.all([
    page.click('form#route-form button[type="submit"]'),
    page.waitForResponse((resp) =>
      resp.url().includes("/onehop") && resp.status() === 200
    ),
  ]);

  await page.waitForSelector(".route-card", { timeout: 5000 });

  // Wait for airline filter bar to show up with pills
  await page.waitForSelector("#airlineFilters .filter-pill", { timeout: 5000 });

  // Grab the first real airline code pill (skip "All airlines" if present)
  const code = await page.$$eval(
    "#airlineFilters .filter-pill",
    (buttons) => {
      const codes = buttons
        .map((btn) => btn.textContent.trim())
        .filter((txt) => txt && txt !== "All airlines");
      return codes[0] || "";
    }
  );

  if (!code) {
    throw new Error("No airline code pills found in filter bar.");
  }

  // Click the pill whose text matches that code
  const pillHandles = await page.$$("#airlineFilters .filter-pill");
  for (const handle of pillHandles) {
    const txt = (await (await handle.getProperty("textContent")).jsonValue())
      .trim();
    if (txt === code) {
      await handle.click();
      break;
    }
  }

  // No waitForTimeout – your frontend updates synchronously after click

  // For each route-card, ensure at least one airline-pill contains that code
  const allCardsOk = await page.$$eval(
    ".route-card",
    (cards, code) =>
      cards.length > 0 &&
      cards.every((card) => {
        const pills = Array.from(card.querySelectorAll(".airline-pill"));
        if (!pills.length) return false;
        return pills.some((p) => p.textContent.includes(`(${code})`));
      }),
    code
  );

  if (!allCardsOk) {
    throw new Error(
      `Not all route cards contain at least one airline-pill with code ${code}.`
    );
  }
}
/**
 * 6) NEW: Airport UI view-mode toggle (Cards ↔ JSON).
 */
async function testAirportUiViewModeToggle(page) {
  await page.goto(`${BASE_URL}/airport-ui`, { waitUntil: "networkidle0" });

  await page.type("#airport", "LGA");

  await Promise.all([
    page.click("form#airport-form button[type=submit]"),
    page.waitForResponse((resp) =>
      resp.url().includes("/airport/routes") && resp.status() === 200
    ),
  ]);

  // Start in Cards mode
  await page.waitForSelector(".card", { timeout: 5000 });

  // Switch to JSON
  await page.click('#viewModeTabs .view-tab[data-mode="json"]');
  await page.waitForSelector("#resultsList pre", { timeout: 5000 });

  const jsonText = await page.$eval("#resultsList pre", (pre) => pre.textContent || "");
  if (!jsonText.includes('"airport"') || !jsonText.includes('"airlines"')) {
    throw new Error("Airport JSON view does not contain expected keys.");
  }

  // Switch back to Cards
  await page.click('#viewModeTabs .view-tab[data-mode="cards"]');
  await page.waitForSelector(".card", { timeout: 5000 });
}

// --------------------------------------------------
// Main runner
// --------------------------------------------------
(async () => {
  const browser = await puppeteer.launch({
    headless: "new",
  });

  const page = await browser.newPage();

  console.log("==================================================");
  console.log(" Front-End UI Test Harness");
  console.log(" (Make sure the C++ server is running on 8080)");
  console.log("==================================================");

  try {
    await runTest("Route Finder loads and shows results", testRouteFinderLoads, page);
    await runTest("Route Finder JSON tab works", testRouteFinderJsonTab, page);
    await runTest("Airline UI shows AA airports", testAirlineUiAA, page);
    await runTest("Airport UI shows airlines at LGA", testAirportUiLGA, page);
    await runTest("Route Finder airline filter pills work", testRouteFinderAirlineFilterPills, page);
    await runTest("Airport UI view-mode toggle works", testAirportUiViewModeToggle, page);

    console.log("\n==================================================");
    console.log("✅ All front-end tests passed!");
    console.log("==================================================");
  } catch (err) {
    console.log("\n==================================================");
    console.log("❌ One or more front-end tests FAILED.");
    console.log("==================================================");
    process.exitCode = 1;
  } finally {
    await browser.close();
  }
})();