// test_frontend.mjs
import puppeteer from "puppeteer";

const BASE_URL = "http://localhost:8080";

let failures = 0;

async function runTest(name, fn) {
  process.stdout.write(`\n[TEST] ${name} ... `);
  try {
    await fn();
    console.log("✅ PASS");
  } catch (err) {
    failures++;
    console.log("❌ FAIL");
    console.error("   ", err.message || err);
  }
}

async function main() {
  // IMPORTANT: start your C++ server in Xcode *before* running this script.
  const browser = await puppeteer.launch();
  const page = await browser.newPage();

  // -------- Test 1: main route finder page "/" ----------
  await runTest("Route Finder loads and shows results", async () => {
    await page.goto(`${BASE_URL}/`, { waitUntil: "networkidle2" });

    // Basic sanity: page title text exists
    const heading = await page.$eval("h1", el => el.textContent.trim());
    if (!heading.includes("OpenFlights Route Finder")) {
      throw new Error(`Unexpected heading: "${heading}"`);
    }

    // Fill in form: SFO -> JFK, limit 5
    await page.type("#src", "SFO");
    await page.type("#dst", "JFK");
    await page.click("button[type=submit]"); // "Search One-Hop Routes"

    // Wait for some route cards to appear
    await page.waitForSelector(".route-card", { timeout: 10000 });

    const cardCount = await page.$$eval(".route-card", els => els.length);
    if (cardCount === 0) {
      throw new Error("No .route-card elements found after search");
    }
  });

  // -------- Test 2: JSON view toggle on "/" ----------
  await runTest("Route Finder JSON tab works", async () => {
    await page.goto(`${BASE_URL}/`, { waitUntil: "networkidle2" });

    // Run a search first
    await page.type("#src", "SFO");
    await page.type("#dst", "JFK");
    await page.click("button[type=submit]");
    await page.waitForSelector(".route-card", { timeout: 10000 });

    // Click JSON tab
    await page.click('.view-tab[data-mode="json"]');

    // Wait for <pre> with JSON
    await page.waitForSelector("pre", { timeout: 10000 });
    const jsonText = await page.$eval("pre", el => el.textContent);
    if (!jsonText.includes('"source"') || !jsonText.includes('"destination"')) {
      throw new Error("JSON view does not look like one-hop JSON");
    }
  });

  // -------- Test 3: Airline UI ----------
  await runTest("Airline UI shows AA airports", async () => {
    await page.goto(`${BASE_URL}/airline-ui`, { waitUntil: "networkidle2" });

    const heading = await page.$eval("h1", el => el.textContent.trim());
    if (!heading.includes("Airline Routes Explorer")) {
      throw new Error(`Unexpected heading: "${heading}"`);
    }

    // Enter AA and submit
    await page.type("#airline", "AA");
    await page.click("#airline-form button[type=submit]");

    await page.waitForSelector(".route-card", { timeout: 10000 });

    // First card should be the airline summary with American Airlines (AA)
    const summaryText = await page.$eval(".route-card .route-main-line", el =>
      el.textContent
    );
    if (!summaryText.includes("American Airlines") || !summaryText.includes("(AA)")) {
      throw new Error("Summary card does not show American Airlines (AA)");
    }
  });

  // -------- Test 4: Airport UI ----------
  await runTest("Airport UI shows airlines at LGA", async () => {
    await page.goto(`${BASE_URL}/airport-ui`, { waitUntil: "networkidle2" });

    const heading = await page.$eval("h1", el => el.textContent.trim());
    if (!heading.includes("Airport Airlines Explorer")) {
      throw new Error(`Unexpected heading: "${heading}"`);
    }

    // Enter LGA and submit
    await page.type("#airport", "LGA");
    await page.click("#airport-form button[type=submit]");

    await page.waitForSelector(".route-card", { timeout: 10000 });

    const airlineRowCount = await page.$$eval(".route-card", els => els.length);
    if (airlineRowCount === 0) {
      throw new Error("No airline rows rendered for LGA");
    }

    // Make sure at least one airline pill exists for an airline row
    const hasPill = await page.$$eval(
      ".route-card .airline-pill",
      els => els.length > 0
    );
    if (!hasPill) {
      throw new Error("No .airline-pill elements found under airline rows");
    }
  });

  // -------- Wrap up ----------
  await browser.close();

  console.log("\n==================================================");
  if (failures === 0) {
    console.log("✅ All front-end tests passed!");
    process.exit(0);
  } else {
    console.log(`❌ ${failures} test(s) failed`);
    process.exit(1);
  }
}

main().catch(err => {
  console.error("Fatal error in test harness:", err);
  process.exit(1);
});
