import { test, expect } from '@playwright/test';

//
// Helper: small delay for autocomplete / network
//
async function waitForNetworkIdle(page) {
  await page.waitForTimeout(500); // slightly generous; tweak if needed
}

test.describe('OpenFlights Route Finder', () => {
  test('home page loads and autocomplete + clear filters work', async ({ page }) => {
    // Go to home page
    await page.goto('/');

    // Basic sanity: title / header present
    await expect(page.getByText('Route Finder', { exact: false })).toBeVisible();

    // Grab inputs (adapt selectors if your IDs differ)
    const fromInput = page.locator('#src-airport, input[name="src"], input[placeholder*="From"]');
    const toInput   = page.locator('#dst-airport, input[name="dst"], input[placeholder*="To"]');

    // Type into "From" and trigger autocomplete
    await fromInput.fill('san');
    await waitForNetworkIdle(page);

    // 👉 Only look at the *source* suggestions list
    const fromSuggestions = page.locator('#src-suggestions');
    await expect(fromSuggestions).toBeVisible();

    // Click the first suggestion if it exists
    const firstSuggestion = fromSuggestions.locator('.autocomplete-item, div, li').first();
    if (await firstSuggestion.isVisible()) {
      const suggestedText = await firstSuggestion.textContent();
      await firstSuggestion.click();

      // After click, the input should now contain something non-empty
      const finalVal = await fromInput.inputValue();
      expect(finalVal).not.toBe('');
      expect(finalVal.length).toBeLessThanOrEqual(40); // still allow "SAN - San Diego" style
    }

    // Type into "To" field for autocomplete
    await toInput.fill('roc');
    await waitForNetworkIdle(page);

    // 👉 Only look at the *destination* suggestions list
    const toSuggestions = page.locator('#dst-suggestions');
    await expect(toSuggestions).toBeVisible();

    // Choose a suggestion again
    const firstToSuggestion = toSuggestions.locator('.autocomplete-item, div, li').first();
    if (await firstToSuggestion.isVisible()) {
      await firstToSuggestion.click();
    }

     // Now test CLEAR FILTERS
    const clearBtn = page.getByRole('button', { name: /clear filters/i });
    await expect(clearBtn).toBeVisible();

    // Capture values before clearing
    const fromBefore = await fromInput.inputValue();
    const toBefore   = await toInput.inputValue();

    await clearBtn.click();

    // After clear, the airports should stay the same
    expect(await fromInput.inputValue()).toBe(fromBefore);
    expect(await toInput.inputValue()).toBe(toBefore);

    // (Optional) you *could* also later add expectations that airline
    // checkboxes are unchecked, layover select reset, etc.
  });


test('route filters change results and clear filters restores', async ({ page }) => {
  await page.goto('/');

  // --- 1) Pick SAN → ROC using the same autocomplete behavior ---

  const fromInput = page.locator('#src-airport, input[name="src"], input[placeholder*="From"]');
  const toInput   = page.locator('#dst-airport, input[name="dst"], input[placeholder*="To"]');

  // FROM: SAN
  await fromInput.fill('san');
  await waitForNetworkIdle(page);

  const fromSuggestions = page.locator('#src-suggestions');
  await expect(fromSuggestions).toBeVisible();

  const firstFromSuggestion = fromSuggestions.locator('.autocomplete-item, div, li').first();
  await firstFromSuggestion.click();

  // TO: ROC
  await toInput.fill('roc');
  await waitForNetworkIdle(page);

  const toSuggestions = page.locator('#dst-suggestions');
  await expect(toSuggestions).toBeVisible();

  const firstToSuggestion = toSuggestions.locator('.autocomplete-item, div, li').first();
  await firstToSuggestion.click();

  // --- 2) Run the one-hop search so we have routes on screen ---

  const oneHopBtn = page.getByRole('button', { name: /search one-hop routes/i });
  await expect(oneHopBtn).toBeVisible();
  await oneHopBtn.click();

  await waitForNetworkIdle(page);

  // Route cards should be present
  const routeCards = page.locator('.route-card');
  const initialCount = await routeCards.count();
  expect(initialCount).toBeGreaterThan(0);

  // --- 3) Turn on a layover filter (e.g., pick a specific via airport) ---

  // The layover filters bar is where the "via ___" chips live
  const layoverFilterChips = page.locator(
    '#layoverFilters button, #layoverFilters .filter-chip, #layoverFilters .filter-pill'
  );

  // Make sure at least one chip exists
  await expect(layoverFilterChips.first()).toBeVisible();

  // Click the first layover chip
  await layoverFilterChips.first().click();
  await waitForNetworkIdle(page);

  const filteredCount = await routeCards.count();

  // After applying a layover filter, we expect the list to be
  // either smaller or at least not *bigger* than before.
  expect(filteredCount).toBeGreaterThan(0);
  expect(filteredCount).toBeLessThanOrEqual(initialCount);

  // --- 4) Clear Filters and confirm results go back to original set ---

  const clearBtn = page.getByRole('button', { name: /clear filters/i });
  await expect(clearBtn).toBeVisible();
  await clearBtn.click();

  await waitForNetworkIdle(page);

  const restoredCount = await routeCards.count();

  // After clearing, we expect to be back to the original result set size
  expect(restoredCount).toBe(initialCount);
});
});

test.describe('Airport UI', () => {
  test('shows summary card even when airport has no airlines in dataset (e.g. SAC)', async ({ page }) => {
    await page.goto('/airport-ui');

    // Check header
    await expect(page.getByText('Airport Airlines Explorer')).toBeVisible();

    const airportInput = page.locator('#airport');
    const limitInput   = page.locator('#limit');
    const submitBtn    = page.getByRole('button', { name: /search airport airlines/i });

    // Use SAC (your edited airport with no routes)
    await airportInput.fill('SAC');
    await limitInput.fill('50');

    await submitBtn.click();

    // Wait for API + rendering
    await waitForNetworkIdle(page);

    // There should be a summary card with the airport name / code
    const summaryCard = page.locator('.card').first();
    await expect(summaryCard).toBeVisible();
    await expect(summaryCard.getByText(/SAC/)).toBeVisible();

    // It may show "No airlines in the dataset" instead of airline cards
    const noAirlines = page.getByText(/no airlines in the dataset/i);
    const airlineCards = page.locator('.route-card');

    // At least one of these must be true:
    if (await noAirlines.count() === 0) {
      // If not showing the message, we should at least not crash, and 
      // either 0 airline cards or some well-formed cards are shown.
      // Just check that the page has some content under "Results".
      const resultsList = page.locator('#resultsList');
      await expect(resultsList).toBeVisible();
    } else {
      await expect(noAirlines).toBeVisible();
      // Optionally ensure no airline cards exist
      await expect(airlineCards).toHaveCount(0);
    }
  });

  test('airport with airlines (e.g. SFO) renders cards clickable and expandable', async ({ page }) => {
    await page.goto('/airport-ui');

    const airportInput = page.locator('#airport');
    const limitInput   = page.locator('#limit');
    const submitBtn    = page.getByRole('button', { name: /search airport airlines/i });

    await airportInput.fill('SFO');
    await limitInput.fill('20');
    await submitBtn.click();

    await waitForNetworkIdle(page);

    // Should have summary + some route-cards
    const summaryCard = page.locator('.card').first();
    await expect(summaryCard).toBeVisible();

    const airlineCard = page.locator('.route-card').first();
    await expect(airlineCard).toBeVisible();

    // Click card to expand details (route table)
    await airlineCard.click();
    const details = airlineCard.locator('.airline-details, .airport-details, .airport-details-grid');
    // We just want *something* to appear inside
    await expect(details.first()).toBeVisible();
  });
});

test.describe('Airline UI', () => {
  test('autocomplete + cards render', async ({ page }) => {
    await page.goto('/airline-ui');

    await expect(page.getByText('Airline Routes Explorer')).toBeVisible();

    const airlineInput = page.locator('#airline');
    const limitInput   = page.locator('#limit');
    const submitBtn    = page.getByRole('button', { name: /search airline routes/i });

    // Type "American"
    await airlineInput.fill('American');
    await waitForNetworkIdle(page);

    const suggestions = page.locator('#airline-suggestions, .autocomplete-list');
    if (await suggestions.isVisible()) {
      await suggestions.locator('div, li').first().click();
    }

    await limitInput.fill('20');
    await submitBtn.click();
    await waitForNetworkIdle(page);

    const routeCards = page.locator('.route-card');
    await expect(routeCards.first()).toBeVisible();
  });
});
